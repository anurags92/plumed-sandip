/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2012 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed-code.org for more information.

   This file is part of plumed, version 2.0.

   plumed is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   plumed is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with plumed.  If not, see <http://www.gnu.org/licenses/>.
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

#include "Analysis.h"
#include "core/ActionSet.h"
#include "core/ActionWithValue.h"
#include "core/PlumedMain.h"
#include "core/Atoms.h"
#include "tools/IFile.h"

namespace PLMD {
namespace analysis {

//+PLUMEDOC INTERNAL reweighting
/*
We can use our knowledge of the Boltzmann distribution in the cannonical ensemble to reweight the data
contained in trajectories.  Using this procedure we can take trajectory at temperature \f$T_1\f$ and use it to 
extract probabilities at a different temperature, \f$T_2\f$, using:

\f[
P(s',t) = \frac{ \sum_{t'}^t \delta( s(x) - s' ) \exp\left( +( \left[\frac{1}{T_1} - \frac{1}{T_2}\right] \frac{U(x,t')}{k_B} \right) }{ \sum_t'^t \exp\left( +\left[\frac{1}{T_1} - \frac{1}{T_2}\right] \frac{U(x,t')}{k_B} \right) }
\f]

where \f$U(x,t')\f$ is the potential energy of the system.  Alternatively, if a static or pseudo-static bias \f$V(x,t')\f$ is acting on 
the system we can remove this bias and get the unbiased probability distribution using:

\f[
P(s',t) = \frac{ \sum_{t'}^t \delta( s(x) - s' ) \exp\left( +\frac{V(x,t')}{k_B T} \right) }{ \sum_t'^t \exp\left( +\frac{V(x,t')}{k_B T} \right) } 
\f]

*/
//+ENDPLUMEDOC

void Analysis::registerKeywords( Keywords& keys ){
  Action::registerKeywords( keys );
  ActionPilot::registerKeywords( keys );
  ActionWithArguments::registerKeywords( keys );
  keys.use("ARG");
  keys.add("compulsory","STRIDE","1","the frequency with which data should be stored for analysis");
  keys.addFlag("USE_ALL_DATA",false,"use the data from the entire trajectory to perform the analysis");
  keys.add("compulsory","RUN","the frequency with which to run the analysis algorithm. This is not required if you specify USE_ALL_DATA");
  keys.addFlag("REWEIGHT_BIAS",false,"reweight the data using all the biases acting on the dynamics. For more information see \\ref reweighting.");
  keys.add("optional","TEMP","the system temperature.  This is required if you are reweighting.");
  keys.add("optional","REWEIGHT_TEMP","reweight data from a trajectory at one temperature and output the probability "
                                      "distribution at a second temperature. For more information see \\ref reweighting. "
                                      "This is not possible during postprocessing.");
  keys.addFlag("WRITE_CHECKPOINT",false,"write out a checkpoint so that the analysis can be restarted in a later run");
  keys.add("hidden","REUSE_DATA_FROM","eventually this will allow you to analyse the same set of data multiple times");
  keys.add("hidden","IGNORE_REWEIGHTING","this allows you to ignore any reweighting factors");
  keys.reserveFlag("NOMEMORY",false,"analyse each block of data separately");
}

Analysis::Analysis(const ActionOptions&ao):
Action(ao),
ActionPilot(ao),
ActionWithArguments(ao),
single_run(false),
nomemory(true),
write_chq(false),
reusing_data(false),
ignore_reweight(false),
needeng(false),
idata(0),
firstAnalysisDone(false),
old_norm(0.0)
{
  std::string prev_analysis; parse("REUSE_DATA_FROM",prev_analysis);
  if( prev_analysis.length()>0 ){
      reusing_data=true;
      mydatastash=plumed.getActionSet().selectWithLabel<Analysis*>( prev_analysis );
      if( !mydatastash ) error("could not find analysis action named " + prev_analysis );
      parseFlag("IGNORE_REWEIGHTING",ignore_reweight);
      if( ignore_reweight ) log.printf("  reusing data stored by %s but ignoring all reweighting\n",prev_analysis.c_str() );
      else log.printf("  reusing data stored by %s\n",prev_analysis.c_str() ); 
  } else { 
      if( keywords.exists("REWEIGHT_BIAS") ){
         bool dobias; parseFlag("REWEIGHT_BIAS",dobias);
         if( dobias ){
             std::vector<ActionWithValue*> all=plumed.getActionSet().select<ActionWithValue*>();
             if( all.empty() ) error("your input file is not telling plumed to calculate anything");
             std::vector<Value*> arg( getArguments() );
             log.printf("  reweigting using the following biases ");
             for(unsigned j=0;j<all.size();j++){
                 std::string flab; flab=all[j]->getLabel() + ".bias";
                 if( all[j]->exists(flab) ){ 
                    biases.push_back( all[j]->copyOutput(flab) ); 
                    arg.push_back( all[j]->copyOutput(flab) ); 
                    log.printf(" %s",flab.c_str()); 
                 }
             }
             log.printf("\n");
             if( biases.empty() ) error("you are asking to reweight bias but there does not appear to be a bias acting on your system");
             requestArguments( arg );
         }
      }

      simtemp=0.; parse("TEMP",simtemp);
      if( simtemp==0 && !biases.empty() ) error("to reweight you must specify a temperature use TEMP");
      rtemp=0; 
      if( keywords.exists("REWEIGHT_TEMP") ) parse("REWEIGHT_TEMP",rtemp);
      if( rtemp!=0 ){
          if( simtemp==0 ) error("to reweight you must specify a temperature use TEMP");
          needeng=true;
          log.printf("  reweighting simulation at %f to probabilities at temperature %f\n",simtemp,rtemp);
      }

      parseFlag("USE_ALL_DATA",single_run); 
      if( !single_run ){
          parse("RUN",freq );
          log.printf("  running analysis every %u steps\n",freq);
          if( freq%getStride()!= 0 ) error("Frequncy of running is not a multiple of the stride");
          ndata=std::floor(freq/getStride() );
          data.resize( ndata );
          for(unsigned i=0;i<ndata;++i){ data[i].resize( getNumberOfArguments() ); }
          logweights.resize( ndata );
          weights.resize( ndata );
      } else {       
          log.printf("  analyzing all data in trajectory\n");
          args.resize( getNumberOfArguments() );
      }
      if( keywords.exists("NOMEMORY") ){ nomemory=false; parseFlag("NOMEMORY",nomemory); }
      if(nomemory) log.printf("  doing a separate analysis for each block of data\n");
      parseFlag("WRITE_CHECKPOINT",write_chq);
      if( write_chq && single_run ){
          write_chq=false;
          warning("ignoring WRITE_CHECKPOINT flag because we are analyzing all data");
      }

      // We need no restart file if we are just collecting data and analyzing all of it
      std::string filename = getName() + "_" + getLabel() + ".chkpnt"; 
      if( write_chq ) rfile.link(*this);
      if( plumed.getRestart() ){
          if( single_run ) error("cannot restart histogram when using the USE_ALL_DATA option");
          if( !write_chq ) warning("restarting without writing a checkpoint file is somewhat strange");
          // Read in data from input file
          readDataFromFile( filename );
          // Setup the restart file (append mode)
          if( write_chq ) rfile.open( filename.c_str(), "aw" );
          // Run the analysis if we stoped in the middle of it last time
          if( idata==ndata ) runAnalysis();
          log.printf("  restarting analysis with %u points read from restart file\n",idata);
      } else if( write_chq ){
          // Setup the restart file (delete any old one)
          remove( filename.c_str() ); 
          rfile.open( filename.c_str(), "w+" );
      }
      if( write_chq ){
         rfile.addConstantField("old_normalization");
         for(unsigned i=0;i<getNumberOfArguments();++i) rfile.setupPrintValue( getPntrToArgument(i) );
      }
  }
}

void Analysis::readDataFromFile( const std::string& filename ){
  double tstep, oldtstep; IFile ifile;
  if( !ifile.FileExist(filename) ) error("failed to find required restart file " + filename );
  ifile.open(filename);

  bool first=true;
  while(ifile.scanField("time",tstep)){
     if( !first && ((tstep-oldtstep) - getStride()*plumed.getAtoms().getTimeStep())>plumed.getAtoms().getTimeStep() ){
         error("frequency of data storage in " + filename + " is not equal to frequency of data storage plumed.dat file");
     }
     for(unsigned j=0;j<getNumberOfArguments();++j){
         ifile.scanField( getPntrToArgument(j)->getName(), data[idata][j] );
     }
     ifile.scanField("log_weight",logweights[idata]);
     ifile.scanField("old_normalization",old_norm);
     ifile.scanField();
     idata++; first=false; oldtstep=tstep;
  }
  ifile.close();
  if(old_norm>0) firstAnalysisDone=true;
}

void Analysis::prepare(){
  if(needeng) plumed.getAtoms().setCollectEnergy(true);
}

void Analysis::calculate(){
  // Don't store the first step (also don't store if we are getting data from elsewhere)
  if( getStep()==0 || reusing_data ) return;
  // Retrieve the bias
  double bias=0.0; for(unsigned i=0;i<biases.size();++i) bias+=biases[i]->get();

  double ww=0;
  if(needeng){
     double energy=plumed.getAtoms().getEnergy()+bias;
     // Reweighting because of temperature difference
     ww=-( (1.0/rtemp) - (1.0/simtemp) )*(energy+bias) / plumed.getAtoms().getKBoltzmann();
     // Reweighting because of biases
     if( !biases.empty() ) ww += bias/( plumed.getAtoms().getKBoltzmann()*simtemp );
  }

  if(single_run){
     // Get the arguments and store them in a vector of vectors
     for(unsigned i=0;i<getNumberOfArguments();++i) args[i]=getArgument(i);
     data.push_back( args );
     logweights.push_back(ww);
  } else {
     // Get the arguments and store them in a vector of vectors
     for(unsigned i=0;i<getNumberOfArguments();++i) data[idata][i]=getArgument(i);
     logweights[idata] = ww; 
  }
  // Write data to checkpoint file
  if( write_chq ) {
      rfile.printField("time",getTime()); rfile.printField("old_normalization",old_norm);
      for(unsigned i=0;i<getNumberOfArguments();++i) rfile.printField( getPntrToArgument(i), getArgument(i) );
      rfile.printField("log_weight",logweights[idata]); rfile.printField();
  }
  // Increment data counter
  idata++;
}

Analysis::~Analysis(){
  if( write_chq ) rfile.close();
}

void Analysis::finalizeWeights( const bool& ignore_weights ){
  // Check that we have the correct ammount of data
  if( !reusing_data && idata!=logweights.size() ) error("something has gone wrong.  Am trying to run analysis but I don't have sufficient data");
  if( weights.size()!=logweights.size() ) weights.resize( logweights.size() );

  norm=0;  // Reset normalization constant
  if( ignore_weights ){
      for(unsigned i=0;i<logweights.size();++i){
          weights[i]=1.0; norm+=1.0;
      } 
  } else if( nomemory ){
      // Find the maximum weight
      double maxweight=logweights[0];
      for(unsigned i=1;i<getNumberOfDataPoints();++i){
         if(logweights[i]>maxweight) maxweight=logweights[i];
      }
      // Calculate normalization constant
      for(unsigned i=0;i<logweights.size();++i){
         norm+=exp( logweights[i]-maxweight );
      }
      // Calculate weights (no memory)
      for(unsigned i=0;i<logweights.size();++i){
          weights[i]=exp( logweights[i]-maxweight );
      }
  // Calculate normalized weights (with memory)
  } else {
      // Calculate normalization constant
      for(unsigned i=0;i<logweights.size();++i){
         norm+=exp( logweights[i] );
      }
      if( !firstAnalysisDone ) old_norm=1.0;
      // Calculate weights (with memory)
      for(unsigned i=0;i<logweights.size();++i){
          weights[i] = exp( logweights[i] ) / old_norm;
      }
      if( !firstAnalysisDone ) old_norm=0.0;
  }
}

void Analysis::runAnalysis(){

  // close the restart file so it is flushed
  if( write_chq ) rfile.close();   

  // Note : could add multiple walkers here - simply read in the data from all
  // other walkers here if we are writing the check points.

  // Calculate the final weights from the log weights 
  if( !reusing_data ){ 
     finalizeWeights( ignore_reweight ); 
  } else {
     mydatastash->finalizeWeights( ignore_reweight );
     norm=mydatastash->retrieveNorm();
  }
  // And run the analysis
  performAnalysis(); idata=0;
  // Update total normalization constant
  old_norm+=norm; firstAnalysisDone=true;

  // Delete the checkpoint file
  if( write_chq ){
     std::string filename = getName() + "_" + getLabel() + ".chkpnt";
     if( remove( filename.c_str() )!=0  ) warning("problem deleting checkpoint file " + filename );
     // If we are running more than one calculation only reopen the restart file
     if( !single_run ) rfile.open( filename.c_str(), "w+" ); 
  }
}

double Analysis::getNormalization() const {
  if( nomemory || !firstAnalysisDone ) return norm;
  return ( 1. + norm/old_norm );
}

void Analysis::update(){
  if( !single_run ){
    if( getStep()>0 && getStep()%freq==0 ) runAnalysis(); 
    else if( idata==logweights.size() ) error("something has gone wrong. Probably a wrong initial time on restart"); 
  }
}

bool Analysis::getPeriodicityInformation(const unsigned& i, std::string& dmin, std::string& dmax){
  bool isperiodic=getPntrToArgument(i)->isPeriodic();
  if(isperiodic) getPntrToArgument(i)->getDomain(dmin,dmax);
  return isperiodic;
}

void Analysis::runFinalJobs() {
  if( !single_run ) return;
  if( getNumberOfDataPoints()==0 ) error("no data is available for analysis");
  runAnalysis(); 
}

std::string Analysis::saveResultsFromPreviousAnalyses( const std::string & filename ){
  FILE* ff=std::fopen( filename.c_str() ,"r");
  // Perhaps replace this with an warning and a backup at some stage
  if(ff && !firstAnalysisDone) error("found file named " + filename + " from previous calculation");
  FILE* fff=NULL; std::string num, backup;
  if( ff ){
     FILE* fff=NULL;
     for(unsigned i=1;;i++){
         Tools::convert(i,num);
         backup=filename + "." + num;
         fff=std::fopen(backup.c_str(),"r");
         if(!fff) break;
     }   
     int check=rename(filename.c_str(), backup.c_str() );
     plumed_massert(check==0,"renaming " + filename + " to " + backup + " failed");
  } else {
     return "";
  }
  if(ff) fclose(ff);
  if(fff) fclose(fff); 
  return backup; 
} 

}
}