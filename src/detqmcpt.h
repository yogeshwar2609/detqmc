/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  See the enclosed file LICENSE for a copy or if
 * that was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2017 Max H. Gerlach
 * 
 * */

// Parallel Tempering Determinantal QMC simulation handling

#ifndef MPIDETQMCPT_H_               
#define MPIDETQMCPT_H_

#include <vector>
#include <queue>
#include <functional>
#include <numeric>              // std::iota
#include <algorithm>            // std::copy
#include <memory>
#include <cstdlib>
#include <limits>
#include <ctime>
#include <functional>
#include <fstream>
#include <armadillo>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#include "boost/preprocessor/comma.hpp"
#include "boost/timer/timer.hpp"
#include "boost/serialization/split_member.hpp"
#include "boost/serialization/string.hpp"
#include "boost/serialization/vector.hpp"
#include "boost/assign/std/vector.hpp"
#include "boost/filesystem.hpp"
#include "boost/archive/binary_oarchive.hpp"
#include "boost/archive/binary_iarchive.hpp"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "boost/mpi.hpp"
#pragma GCC diagnostic pop
#include "metadata.h"
#include "datamapwriter.h"
#include "detqmcparams.h"
#include "detmodelparams.h"
#include "detmodelloggingparams.h"
#include "detmodel.h"
#include "mpiobservablehandlerpt.h"
#include "rngwrapper.h"
#include "exceptions.h"
#include "tools.h"
#include "git-revision.h"
#include "timing.h"



// Class handling the simulation
template<class Model, class ModelParams = ModelParams<Model> >
class DetQMCPT {
public:
    //constructor to init a new simulation:
    DetQMCPT(const ModelParams& parsmodel, const DetQMCParams& parsmc,
             const DetQMCPTParams& parspt,
             const DetModelLoggingParams& loggingParams = DetModelLoggingParams());

    //constructor to resume a simulation from a dumped state file:
    //we allow to change some MC parameters at this point:
    //  sweeps & saveInterval
    //if values > than the old values are specified, change them
    DetQMCPT(const std::string& stateFileName, const DetQMCParams& newParsmc);


    //carry out simulation determined by parsmc and parspt given in construction,
    //- handle thermalization & measurement stages as necessary
    //- save state and results periodically
    //- if granted walltime is almost over, save state & results
    //  and exit gracefully
    void run();

    // update results stored on disk
    void saveResults();
    // dump simulation parameters and the current state to a Boost::S11n archive,
    // also write out information about the current simulation state to info.dat
    void saveState();

    virtual ~DetQMCPT();
protected:
    //helper for constructors -- set all parameters and initialize contained objects
    void initFromParameters(const ModelParams& parsmodel, const DetQMCParams& parsmc,
                            const DetQMCPTParams& parspt,
                            const DetModelLoggingParams& loggingParams = DetModelLoggingParams());

    void replicaExchangeStep();
    void replicaExchangeConsistencyCheck(); // verify that processes have the right control parameters

    void saveReplicaExchangeStatistics();

    // subdirectory currently associated with replica set to the control parameter with index cpi
    std::string control_parameter_subdir(int cpi);
    
    ModelParams parsmodel;
    DetQMCParams parsmc;
    DetQMCPTParams parspt;
    DetModelLoggingParams parslogging;
    typedef DetQMCParams::GreenUpdateType GreenUpdateType;
    
    MetadataMap modelMeta;
    MetadataMap mcMeta;
    MetadataMap ptMeta;    
    RngWrapper rng;
    std::unique_ptr<Model> replica;
    typedef std::unique_ptr<ScalarObservableHandlerPT> ObsPtr;
    typedef std::unique_ptr<VectorObservableHandlerPT> VecObsPtr;
    std::vector<ObsPtr> obsHandlers;
    std::vector<VecObsPtr> vecObsHandlers;      //need to be pointers: holds both KeyValueObservableHandlerPTs and VectorObservableHandlerPTs
    uint32_t sweepsDone;                        //Measurement sweeps done
    uint32_t sweepsDoneThermalization;          //thermalization sweeps done

    uint32_t swCounter; //helper counter in run() -- e.g. sweeps between measurements -- should also be serialized

    boost::timer::cpu_timer elapsedTimer;           //during this simulation run
    uint32_t curWalltimeSecs() {
        return static_cast<uint32_t>(elapsedTimer.elapsed().wall / 1000 / 1000 / 1000); // ns->mus->ms->s
    }
    uint32_t totalWalltimeSecs; //this is serialized and carries the elapsed walltime in seconds
                                //accumulated over all runs, updated on call of saveResults()
    uint32_t walltimeSecsLastSaveResults; //timer seconds at previous saveResults() call --> used to update totalWalltimeSecs
    uint32_t grantedWalltimeSecs; //walltime the simulation is allowed to run
    std::string jobid; //id string from the job scheduling system, or "nojobid"

    //MPI specifics:
    int numProcesses;      //total number of parallel processes
    int processIndex;      //MPI-rank of the current process

    int local_current_parameter_index; // current control parameter index of this process's replica

    // specific to the root process
    std::vector<int> current_process_par; // indexed by process rank number, giving control parameter index
                                          // currently associated to the replica at that process
    std::vector<int> current_par_process; // the reverse association
    std::vector<double> exchange_action;       // for each replica: its locally measured exchange action
    // for control-parameter specific data, e.g. MC stepsize adjustment
    //std::vector<uint8_tString> control_data_buffer_1, control_data_buffer_2;
    std::vector<std::string> process_control_data_buffer; // map process index -> control data
    // for each process:
    std::string local_control_data_buffer;

    //Statistics about replica exchange
    // -- evaluate at rank 0 --
    struct ExchangeStatistics {
        // exchange acceptance ratios histogram
        std::vector<int> par_swapUpAccepted; // count for each control parameter index how often replica exchanges
                                                  // with one index higher are accepted
        std::vector<int> par_swapUpProposed; // .. are proposed
        // replica diffusion -- histograms if replicas (processes) are
        // moving up or down in control parameter space
        enum ParameterDirection {NONE_P, UP_P, DOWN_P};
        std::vector<ParameterDirection> process_goingWhere; // track for each replica current direction
        std::vector<int> par_countGoingUp;//at each attempted parameter swap: if replica at current parameter has visited parMax last (and not parMin) increase
        std::vector<int> par_countGoingDown; //vice versa for replicas having visited parMin last (and not parMax)

        ExchangeStatistics() :
            par_swapUpAccepted(), par_swapUpProposed(), process_goingWhere(),
            par_countGoingUp(), par_countGoingDown()
            { }
        
        ExchangeStatistics(const DetQMCPTParams& ptPars) :
            par_swapUpAccepted(ptPars.controlParameterValues.size(), 0),
            par_swapUpProposed(ptPars.controlParameterValues.size(), 0),
            process_goingWhere(ptPars.controlParameterValues.size(), NONE_P),
            par_countGoingUp(ptPars.controlParameterValues.size(), 0),
            par_countGoingDown(ptPars.controlParameterValues.size(), 0)
            { }

        template<class Archive>
        void serialize(Archive& ar, const uint32_t /*version*/) {
            ar & par_swapUpAccepted & par_swapUpProposed & process_goingWhere
               & par_countGoingUp & par_countGoingDown;
        }        
    } es;


    // Saving system configurations in parallelized simulations:
    // -- root process collects configs from all replicas, saves all of them after buffering
    struct SaveConfigurations {
        typedef typename Model::SystemConfig SystemConfig;
        std::queue<SystemConfig> local_bufferedConfigurations; // each process buffers the configs of its own replica here (FIFO)
        std::queue<int> local_bufferedControlParameterIndex;   // each process buffers the current controlParameterIndex for its replica
        std::string local_mpi_buffer;

        // at rank 0:
        typedef typename Model::SystemConfig_FileHandle FileHandle;
        std::vector<FileHandle> par_fileHandle; // file handle to save configurations for each control parameter index
        std::vector<std::string> process_mpi_buffer; // buffer for MPI gathered data for each process index
        std::vector<int> process_controlParameterIndex; // cpi mathching the system configuration in process_mpi_buffer
    } sc;
    void setup_SaveConfigurations();
    void buffer_local_system_configuration();
    void gather_and_output_buffered_system_configurations();
private:
    //Serialize only the content data that has changed after construction.
    //Only call for deserialization after DetQMCPT has already been constructed and initialized!

    //separate functions loadContents, saveContents; both employ serializeContentsCommon
    template<class Archive>
    void loadContents(Archive& ar) {
        serializeContentsCommon(ar);

        replica->loadContents(ar);

        // distribute and update control parameter for replica
        // after deserialization
        namespace mpi = boost::mpi;
        mpi::communicator world;        
        int new_param_index = 0;
        mpi::scatter(world,
                     current_process_par, // send
                     new_param_index,     // recv
                     0                    // root
            );
        local_current_parameter_index = new_param_index;
        replica->set_exchange_parameter_value(
            parspt.controlParameterValues[new_param_index]
            );        
    }

    template<class Archive>
    void saveContents(Archive& ar) {
        serializeContentsCommon(ar);

        replica->saveContents(ar);
    }


    template<class Archive>
    void serializeContentsCommon(Archive& ar) {
        ar & rng;                   //serialize completely

        for (auto p = obsHandlers.begin(); p != obsHandlers.end(); ++p) {
            //ATM no further derived classes of ScalarObservableHandlerPT have a method serializeContents
            (*p)->serializeContents(ar);
        }
        for (auto p = vecObsHandlers.begin(); p != vecObsHandlers.end(); ++p) {
            //ATM no further derived classes of VectorObservableHandlerPT have a method serializeContents
            (*p)->serializeContents(ar);
        }
        ar & sweepsDone & sweepsDoneThermalization;
        ar & swCounter;

        ar & totalWalltimeSecs;

        ar & local_current_parameter_index;

        ar & current_process_par;

        ar & current_par_process;

        ar & es;                // exchange statistics
    }
};




template<class Model, class ModelParams>
void DetQMCPT<Model,ModelParams>::initFromParameters(const ModelParams& parsmodel_, const DetQMCParams& parsmc_,
                                                     const DetQMCPTParams& parspt_,
                                                     const DetModelLoggingParams& loggingParams /*default argument*/) {
    parsmodel = parsmodel_;
    parsmodel = updateTemperatureParameters(parsmodel);    
    parsmc = parsmc_;
    parspt = parspt_;
    parslogging = loggingParams;

    parsmc.check();
    parspt.check();
    parslogging.check();

    // Set up MPI info
    namespace mpi = boost::mpi;
    mpi::communicator world;
    processIndex = world.rank();
    numProcesses = world.size();
    if (numProcesses != int(parspt.controlParameterValues.size())) {
        throw_ConfigurationError("Number of processes " + numToString(numProcesses) +
                                 " does not match number of control parameter values " +
                                 numToString(parspt.controlParameterValues.size()));
    }

    // set up RNG
    if (parsmc.specified.count("rngSeed") == 0) {
        if (processIndex == 0) {
            std::cout << "No rng seed specified, will use std::time(0) determined at root process" << std::endl;
            parsmc.rngSeed = (uint32_t) std::time(0);
        }
        //unsigned broadcast_rngSeed = parsmc.rngSeed; // work around lacking support for MPI_UINT32_T
        //MPI_Bcast(&broadcast_rngSeed, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD);
        mpi::broadcast(world, parsmc.rngSeed, 0);
    }
    rng = RngWrapper(parsmc.rngSeed, (parsmc.simindex + 1) * (processIndex + 1));

    // set up control parameters for current process replica parameters
    local_current_parameter_index = processIndex;
    parsmodel.set_exchange_parameter_value(
        parspt.controlParameterValues[local_current_parameter_index]);

    parsmodel.check();

    // the replicaLogfiledir is for some consistency checks, and is most likely
    // to be unused in later production runs.  Replicas to be used with this
    // replica-exchange code therefore are required to support this parameter
    // in the createReplica function associated to them.
    std::string replicaLogfiledir = "log_proc_" + numToString(processIndex);
    
    createReplica(replica, rng, parsmodel, parslogging, replicaLogfiledir);
    
    // at rank 0 keep track of which process has which control parameter currently
    // and track exchange action contributions
    // also initialize control parameter data buffers,
    // at rank 0: exchangeStatistics
    if (processIndex == 0) {
        // fill with 0, 1, 2, ... numProcesses-1
        current_process_par.resize(numProcesses);
        std::iota(current_process_par.begin(), current_process_par.end(), 0);
        current_par_process.resize(numProcesses);
        std::iota(current_par_process.begin(), current_par_process.end(), 0);
        exchange_action.resize(numProcesses, 0);
        
        // control_data_buffer_1.resize(numProcesses * replica->get_control_data_buffer_size());        
        // control_data_buffer_2.resize(numProcesses * replica->get_control_data_buffer_size());
        process_control_data_buffer.resize(numProcesses);        
        // control_data_buffer_2.resize(numProcesses);
        es = ExchangeStatistics(parspt);
    }
    //local_control_data_buffer.resize(replica->get_control_data_buffer_size());
    local_control_data_buffer.clear();


    //prepare metadata
    modelMeta = parsmodel.prepareMetadataMap();
    modelMeta.erase(parspt.controlParameterName);
    mcMeta = parsmc.prepareMetadataMap();
    mcMeta.erase("stateFileName");
    ptMeta = parspt.prepareMetadataMap();

    //prepare observable handlers
    auto scalarObs = replica->getScalarObservables();
    for (auto obsP = scalarObs.cbegin(); obsP != scalarObs.cend(); ++obsP) {
        obsHandlers.push_back(
            ObsPtr(new ScalarObservableHandlerPT(*obsP, current_process_par, parsmc, parspt, modelMeta, mcMeta, ptMeta))
        );
    }

    auto vectorObs = replica->getVectorObservables();
    for (auto obsP = vectorObs.cbegin(); obsP != vectorObs.cend(); ++obsP) {
        vecObsHandlers.push_back(
            VecObsPtr(new VectorObservableHandlerPT(*obsP, current_process_par, parsmc, parspt, modelMeta, mcMeta, ptMeta))
        );
    }
    auto keyValueObs = replica->getKeyValueObservables();
    for (auto obsP = keyValueObs.cbegin(); obsP != keyValueObs.cend(); ++obsP) {
        vecObsHandlers.push_back(
            VecObsPtr(new KeyValueObservableHandlerPT(*obsP, current_process_par, parsmc, parspt, modelMeta, mcMeta, ptMeta))
        );
    }

    // setup files for system configuration streams [if files do not exist already]
    // [each process for its local replica]
    // Afterwards only the master process will continue to write to these files
    if (parsmc.saveConfigurationStreamText or parsmc.saveConfigurationStreamBinary) {
        namespace fs = boost::filesystem;
        std::string subdir_string = control_parameter_subdir(local_current_parameter_index);
        fs::create_directories(subdir_string);
        std::string parname = parspt.controlParameterName;
        std::string parvalue = numToString(parspt.controlParameterValues[local_current_parameter_index]);            
        MetadataMap modelMeta_cpi = modelMeta;
        modelMeta_cpi[parname] = parvalue;
        std::string headerInfoText = metadataToString(modelMeta_cpi, "#")
            + metadataToString(mcMeta, "#")
            + metadataToString(ptMeta, "#");
        if (parsmc.saveConfigurationStreamText) {
            replica->saveConfigurationStreamTextHeader(headerInfoText, subdir_string);
        }
        if (parsmc.saveConfigurationStreamBinary) {
            replica->saveConfigurationStreamBinaryHeaderfile(headerInfoText, subdir_string);
        }
    }


    //query allowed walltime
    const char* pbs_walltime = std::getenv("PBS_WALLTIME");
    if (pbs_walltime) {
        grantedWalltimeSecs = fromString<decltype(grantedWalltimeSecs)>(pbs_walltime);
    } else {
        grantedWalltimeSecs = std::numeric_limits<decltype(grantedWalltimeSecs)>::max();
    }
    if (processIndex == 0) {
        std::cout << "Granted walltime: " << grantedWalltimeSecs << " seconds.\n";

        //query SLURM Jobid
        const char* jobid_env = std::getenv("SLURM_JOBID");
        if (jobid_env) {
            jobid = jobid_env;
        } else {
            jobid = "nojobid";
        }
        std::cout << "Job ID: " << jobid << "\n";

        std::cout << "\nSimulation initialized, parameters: " << std::endl;
        std::cout << metadataToString(mcMeta, " ")
                  << metadataToString(ptMeta, " ")
                  << metadataToString(modelMeta, " ") << std::endl;
    }
}



template<class Model, class ModelParams>
DetQMCPT<Model, ModelParams>::DetQMCPT(const ModelParams& parsmodel_, const DetQMCParams& parsmc_,
                                       const DetQMCPTParams& parspt_,
                                       const DetModelLoggingParams& parslogging_ /* default argument */)
    :
    parsmodel(), parsmc(), parspt(), parslogging(),
    //proper initialization of default initialized members done in initFromParameters
    modelMeta(), mcMeta(), ptMeta(), rng(), replica(),
    obsHandlers(), vecObsHandlers(),
    sweepsDone(0), sweepsDoneThermalization(),
    swCounter(0),
    elapsedTimer(),     // start timing
    totalWalltimeSecs(0), walltimeSecsLastSaveResults(0),
    grantedWalltimeSecs(0), jobid(""),
    numProcesses(1), processIndex(0),
    local_current_parameter_index(0),
    current_process_par(1, 0),
    current_par_process(1, 0),
    exchange_action(1, 0),
    process_control_data_buffer(),
    //control_data_buffer_2(),
    local_control_data_buffer(),
    es(), sc()
{
    initFromParameters(parsmodel_, parsmc_, parspt_, parslogging_);
}

template<class Model, class ModelParams>
DetQMCPT<Model, ModelParams>::DetQMCPT(const std::string& stateFileName, const DetQMCParams& newParsmc) :
    parsmodel(), parsmc(), parspt(), parslogging(),
    //proper initialization of default initialized members done by loading from archive
    modelMeta(), mcMeta(), rng(), replica(),
    obsHandlers(), vecObsHandlers(),
    sweepsDone(), sweepsDoneThermalization(),
    swCounter(0),
    elapsedTimer(),     // start timing
    totalWalltimeSecs(0), walltimeSecsLastSaveResults(0),
    grantedWalltimeSecs(0), jobid(""),
    numProcesses(1), processIndex(0),
    local_current_parameter_index(0),
    current_process_par(1, 0),
    current_par_process(1, 0),
    exchange_action(1, 0),
    process_control_data_buffer(),
    //control_data_buffer_2(),
    local_control_data_buffer(),
    es()
{
    std::ifstream ifs;
    ifs.exceptions(std::ifstream::badbit | std::ifstream::failbit);
    ifs.open(stateFileName.c_str(), std::ios::binary);
    boost::archive::binary_iarchive ia(ifs);
    DetModelLoggingParams parslogging_;    
    ModelParams parsmodel_;
    DetQMCParams parsmc_;
    DetQMCPTParams parspt_;
    ia >> parslogging_ >> parsmodel_ >> parsmc_ >> parspt_;

    if (newParsmc.sweeps > parsmc_.sweeps) {
        if (processIndex == 0) {
            std::cout << "Target sweeps will be changed from " << parsmc_.sweeps
                      << " to " << newParsmc.sweeps << std::endl;
        }
        parsmc_.sweeps = newParsmc.sweeps;
        parsmc_.sweepsHasChanged = true;
    }
    if (newParsmc.saveInterval > 0 and newParsmc.saveInterval != parsmc_.saveInterval) {
        if (processIndex == 0) {
            std::cout << "saveInterval will be changed from " << parsmc_.saveInterval
                      << " to " << newParsmc.saveInterval << std::endl;
        }
        parsmc_.saveInterval = newParsmc.saveInterval;
    }
    parsmc_.stateFileName = stateFileName;

    //make sure mcparams are set correctly as "specified"
#define SPECIFIED_INSERT_VAL(x) if (parsmc_.x) { parsmc_.specified.insert(#x); }
#define SPECIFIED_INSERT_STR(x) if (not parsmc_.x.empty()) { parsmc_.specified.insert(#x); }
    SPECIFIED_INSERT_VAL(sweeps);
    SPECIFIED_INSERT_VAL(thermalization);
    SPECIFIED_INSERT_VAL(jkBlocks);
    SPECIFIED_INSERT_VAL(measureInterval);
    SPECIFIED_INSERT_VAL(saveInterval);
    SPECIFIED_INSERT_STR(stateFileName);
#undef SPECIFIED_INSERT_VAL
#undef SPECIFIED_INSERT_STR
    if (not parsmc_.greenUpdateType_string.empty()) {
        parsmc_.specified.insert("greenUpdateType");
    }
    
    initFromParameters(parsmodel_, parsmc_, parspt_, parslogging_);
    loadContents(ia);

    if (processIndex == 0) {
        std::cout << "\n"
                  << "State of previous simulation has been loaded.\n"
                  << "  sweepsDoneThermalization: " << sweepsDoneThermalization << "\n"
                  << "  sweepsDone: " << sweepsDone << std::endl;
    }
}

template<class Model, class ModelParams>
void DetQMCPT<Model, ModelParams>::saveState() {
    timing.start("saveState");

    namespace fs = boost::filesystem;

    //serialize state to file
    // -- every process needs to do this
    std::ofstream ofs;
    ofs.exceptions(std::ofstream::badbit | std::ofstream::failbit);
    ofs.open(parsmc.stateFileName.c_str(), std::ios::binary);
    boost::archive::binary_oarchive oa(ofs);
    oa << parslogging << parsmodel << parsmc << parspt;
    saveContents(oa);

    //write out info about state of simulation to "info.dat"
    // -- one for the main directory and one for each subdirectory (control parameter specific)
    // -- only the master process does this
    if (processIndex == 0) {
        MetadataMap currentState;
        currentState["sweepsDoneThermalization"] = numToString(sweepsDoneThermalization);
        currentState["sweepsDone"] = numToString(sweepsDone);
        
        uint32_t cwts = curWalltimeSecs();
        totalWalltimeSecs += (cwts - walltimeSecsLastSaveResults);
        walltimeSecsLastSaveResults = cwts;
        
        currentState["totalWallTimeSecs"] = numToString(totalWalltimeSecs);

        auto write_info = [this](const MetadataMap& modelMeta_, const MetadataMap& currentState,
                                 const fs::path& subdirectory) {
            fs::create_directories(subdirectory);
            std::string commonInfoFilename = (subdirectory / fs::path("info.dat")).string();
            writeOnlyMetaData(commonInfoFilename, collectVersionInfo(),
                              "Collected information about this determinantal quantum Monte Carlo simulation",
                              false);
            writeOnlyMetaData(commonInfoFilename, modelMeta_,
                              "Model parameters:",
                              true);
            writeOnlyMetaData(commonInfoFilename, mcMeta,
                              "Monte Carlo parameters:",
                              true);
            writeOnlyMetaData(commonInfoFilename, ptMeta,
                              "Replica exchange parameters:",
                              true);
            writeOnlyMetaData(commonInfoFilename, currentState,
                              "Current state of simulation:",
                              true);
        };

        // top level directory: info not restricted to any value of the control parameter
        write_info(modelMeta, currentState, fs::path("."));

        // write a separate info.dat for each value of the control parameter
        for (int cpi = 0; cpi < numProcesses; ++cpi) {
            std::string subdir_string = control_parameter_subdir(cpi);
            std::string parname = parspt.controlParameterName;
            std::string parvalue = numToString(parspt.controlParameterValues[cpi]);            
            MetadataMap modelMeta_cpi = modelMeta;
            modelMeta_cpi[parname] = parvalue;
            write_info(modelMeta_cpi, currentState, fs::path(subdir_string));
        }        
    }

    if (processIndex == 0) {
        saveReplicaExchangeStatistics();
    }

    if (processIndex == 0) {
        std::cout << "State has been saved." << std::endl;
    }

    timing.stop("saveState");
}

template<class Model, class ModelParams>
void DetQMCPT<Model, ModelParams>::saveReplicaExchangeStatistics() {
    IntDoubleMapWriter mapWriter;
    mapWriter.addMetadataMap(modelMeta);
    mapWriter.addMetadataMap(mcMeta);
    mapWriter.addMetadataMap(ptMeta);

    // index -> control parameter
    std::shared_ptr<std::map<int, double>> controlParameters(new std::map<int,double>);
    for (int cpi = 0; cpi < numProcesses; ++cpi) {
        (*controlParameters)[cpi] = parspt.controlParameterValues[cpi];
    }
    IntDoubleMapWriter controlParametersWriter = mapWriter;
    controlParametersWriter.addMeta("key", "control parameter index");    
    controlParametersWriter.addHeaderText("Control parameter values");
    controlParametersWriter.addHeaderText("control parameter index \t control parameter value");
    controlParametersWriter.setData(controlParameters);
    controlParametersWriter.writeToFile("exchange-parameters.values");

    //control parameter swap acceptance
    std::shared_ptr<std::map<int, double>> cpiAccRates(new std::map<int,double>);
    for (int cpi = 0; cpi < numProcesses; ++cpi) {
        int countAccepted = es.par_swapUpAccepted[cpi];
        int countProposed = es.par_swapUpProposed[cpi];
        double ar = 0.0;
        if (countProposed != 0) {
            ar = static_cast<double>(countAccepted)
                / static_cast<double>(countProposed);
        }
        (*cpiAccRates)[cpi] = ar;
    }
    IntDoubleMapWriter cpiAccRatesWriter = mapWriter;
    cpiAccRatesWriter.addMeta("key", "control parameter index");    
    cpiAccRatesWriter.addHeaderText("Acceptance ratio of exchanging replicas at control parameters (upwards)");
    cpiAccRatesWriter.addHeaderText("control parameter index \t acceptance ratio");
    cpiAccRatesWriter.setData(cpiAccRates);
    cpiAccRatesWriter.writeToFile("exchange-acceptance.values");

    //diffusion fraction
    std::shared_ptr<std::map<int, double>> dfractions(new std::map<int,double>);
    for (int cpi = 0; cpi < numProcesses; ++cpi) {
        int countUp = es.par_countGoingUp[cpi];
        int countDown = es.par_countGoingDown[cpi];
        double df = 0.0;
        if (countUp + countDown != 0) {
            df = static_cast<double>(countUp)
                / static_cast<double>(countUp + countDown);
        }
        (*dfractions)[cpi] = df;
    }
    IntDoubleMapWriter dfractionsWriter = mapWriter;
    dfractionsWriter.addMeta("key", "control parameter index");    
    dfractionsWriter.addHeaderText("Diffusion fraction of replicas at control parameters: df = nUp / (nUp + nDown)");
    dfractionsWriter.addHeaderText("control parameter index \t diffusion fraction");
    dfractionsWriter.setData(dfractions);
    dfractionsWriter.writeToFile("exchange-diffusion.values");
}


template<class Model, class ModelParams>
std::string DetQMCPT<Model, ModelParams>::control_parameter_subdir(int cpi) {
    std::string parname = parspt.controlParameterName;
    std::string parvalue = numToString(parspt.controlParameterValues[cpi]);
    std::string subdir_string = "p" + numToString(cpi) + "_" + parname + parvalue;
    return subdir_string;
}



template<class Model, class ModelParams>
DetQMCPT<Model, ModelParams>::~DetQMCPT() {
}

template<class Model, class ModelParams>
void DetQMCPT<Model, ModelParams>::setup_SaveConfigurations() {
    namespace fs = boost::filesystem;
    if (parsmc.saveConfigurationStreamText or parsmc.saveConfigurationStreamBinary) {
        sc = SaveConfigurations();
    
        if (processIndex == 0) {
            sc.par_fileHandle.resize(numProcesses);
            for (int cpi = 0; cpi < numProcesses; ++cpi) {
                std::string subdirectory = control_parameter_subdir(cpi);
                fs::create_directories(subdirectory);
                sc.par_fileHandle[cpi] = replica->prepareSystemConfigurationStreamFileHandle(
                    parsmc.saveConfigurationStreamBinary, parsmc.saveConfigurationStreamText,
                    subdirectory
                    );
            }
            sc.process_mpi_buffer.resize(numProcesses);
            for (int pi = 0; pi < numProcesses; ++pi) {
                sc.process_mpi_buffer[pi].clear();
            }
            sc.process_controlParameterIndex.resize(numProcesses);
        }
    }
}

template<class Model, class ModelParams>
void DetQMCPT<Model, ModelParams>::buffer_local_system_configuration() {
    // each process buffers the system configuration of its replica in local memory
    if (parsmc.saveConfigurationStreamText or parsmc.saveConfigurationStreamBinary) {
        sc.local_bufferedConfigurations.push(replica->getCurrentSystemConfiguration());
        sc.local_bufferedControlParameterIndex.push(local_current_parameter_index);
    }
}

template<class Model, class ModelParams>
void DetQMCPT<Model, ModelParams>::gather_and_output_buffered_system_configurations() {
    namespace mpi = boost::mpi;
    mpi::communicator world;

    if (not (parsmc.saveConfigurationStreamText or parsmc.saveConfigurationStreamBinary)) {
        return;
    }

    while (not sc.local_bufferedConfigurations.empty()) {
        // collect at rank0
        
        sc.local_mpi_buffer.clear();
        serialize_systemConfig_to_buffer(sc.local_mpi_buffer,
                                         sc.local_bufferedConfigurations.front());
        int local_cpi = sc.local_bufferedControlParameterIndex.front();

        sc.local_bufferedConfigurations.pop();
        sc.local_bufferedControlParameterIndex.pop();
        
        if (processIndex == 0) {
            for (auto& datastring : sc.process_mpi_buffer) {
                datastring.clear();
            }
        }
        
        mpi::gather(world,
                    sc.local_mpi_buffer,   // send
                    sc.process_mpi_buffer, // recv at rank 0
                    0);

        mpi::gather(world,
                    local_cpi,                        // send
                    sc.process_controlParameterIndex, // recv at rank 0
                    0);

        // write to the right files

        if (processIndex == 0) {
            for (int pi = 0; pi < numProcesses; ++pi) {
                typename SaveConfigurations::SystemConfig pi_systemConfig;
                deserialize_systemConfig_from_buffer(pi_systemConfig, sc.process_mpi_buffer[pi]);
                int cpi = sc.process_controlParameterIndex[pi];

                pi_systemConfig.write_to_disk(sc.par_fileHandle[cpi]);
            }
        }
    }

    // flush all ofstreams
    if (processIndex == 0) {
        for (int cpi = 0; cpi < numProcesses; ++cpi) {
            sc.par_fileHandle[cpi].flush();
        }
    }
}


template<class Model, class ModelParams>
void DetQMCPT<Model, ModelParams>::run() {
    enum Stage { T, M, F };     //Thermalization, Measurement, Finished
    Stage stage = T;

    //local helper functions to initialize a "stage" of the big loop
    auto thermalizationStage = [&stage, this]() {
        stage = T;
        if (processIndex == 0) {
            std::cout << "Thermalization for " << parsmc.thermalization << " sweeps..." << std::endl;
        }
    };
    auto measurementsStage = [&stage, this]() {
        stage = M;
        if (processIndex == 0) {
            std::cout << "Measurements for " << parsmc.sweeps << " sweeps..." << std::endl;
        }
    };
    auto finishedStage = [&stage, this]() {
        stage = F;
        if (processIndex == 0) {
            std::cout << "Measurements finished\n" << std::endl;
        }
    };
    // helper function for saving state, results, timeseries, configuration streams
    auto save = [&]() {
        if (stage == Stage::M) {
            this->gather_and_output_buffered_system_configurations();
            this->saveResults();
        }
        this->saveState();
    };

    if (parsmc.saveConfigurationStreamText or parsmc.saveConfigurationStreamBinary) {
        setup_SaveConfigurations();
    }
    
    if (sweepsDoneThermalization < parsmc.thermalization) {
        thermalizationStage();
    } else if (sweepsDone < parsmc.sweeps) {
        measurementsStage();
    } else {
        finishedStage();
    }

    const uint32_t SafetyMinutes = 35;

    const std::string abortFilenames[] = { "ABORT." + jobid,
                                           "../ABORT." + jobid,
                                           "ABORT.all",
                                           "../ABORT.all" };

    namespace mpi = boost::mpi;
    mpi::communicator world;

    while (stage != F) {                //big loop
        // do we need to quit?
    	if (swCounter % 2 == 0) {
            char stop_now = false;
            if (processIndex == 0) {
                if (curWalltimeSecs() > grantedWalltimeSecs - SafetyMinutes*60) {
                    std::cout << "Granted walltime will be exceeded in less than " << SafetyMinutes << " minutes.\n";
                    stop_now = true;
                } else {
                    for (auto abortfn : abortFilenames) {
                        if (boost::filesystem::exists(abortfn)) {
                            std::cout << "Found file " << abortfn << ".\n";
                            stop_now = true;
                        }
                    }
                }
            }
            // MPI_Bcast( &stop_now, 1, MPI_CHAR,
            //            0, MPI_COMM_WORLD
            //     );
            mpi::broadcast(world, stop_now, 0);
            if (stop_now) {
                //close to exceeded walltime or we find that a file has been placed,
                //which signals us to abort this run for some other reason.
                //but only save state and exit if we have done an even
                //number of sweeps for ("economic") serialization guarantee [else do one sweep more]
                if (processIndex == 0) {
                    std::cout << "Current stage:\n"
                              << " sweeps done thermalization: " << sweepsDoneThermalization << "\n"
                              << " sweeps done measurements:   " << sweepsDone << "\n";
                    std::cout << "Save state / results and exit gracefully." << std::endl;
                }
                save();
                std::cout << " OK " << std::endl;
                break;  //while
            }
    	}

        //thermalization & measurement stages | main work
        switch (stage) {

        case T: {
            switch(parsmc.greenUpdateType) {
            case GreenUpdateType::GreenUpdateTypeSimple:
                replica->sweepSimpleThermalization();
                break;
            case GreenUpdateType::GreenUpdateTypeStabilized:
                replica->sweepThermalization();
                break;
            }
            ++sweepsDoneThermalization;
            ++swCounter;
            if (swCounter == parsmc.saveInterval) {
                if (processIndex == 0) {
                    std::cout << "  " << sweepsDoneThermalization << " ... saving state...";
                }
                swCounter = 0;
                save();
                //MPI_Barrier(MPI_COMM_WORLD);
                world.barrier();
                if (processIndex == 0) {
                    std::cout << " OK" << std::endl;
                }
            }
            if (sweepsDoneThermalization == parsmc.thermalization) {
                if (processIndex == 0) {
                    std::cout << "Thermalization finished\n" << std::endl;
                }
                replica->thermalizationOver(processIndex);
                swCounter = 0;
                measurementsStage();
            }
            break;  //case T
        }
        case M: {
            ++swCounter;
            bool takeMeasurementNow = (swCounter % parsmc.measureInterval == 0);
            
            switch(parsmc.greenUpdateType) {
            case GreenUpdateType::GreenUpdateTypeSimple:
                replica->sweepSimple(takeMeasurementNow);
                break;
            case GreenUpdateType::GreenUpdateTypeStabilized:
                replica->sweep(takeMeasurementNow);
                break;
            }

            if (takeMeasurementNow) {
                for (auto ph = obsHandlers.begin(); ph != obsHandlers.end(); ++ph) {
                    (*ph)->insertValue(sweepsDone);
                }
                for (auto ph = vecObsHandlers.begin(); ph != vecObsHandlers.end(); ++ph) {
                    (*ph)->insertValue(sweepsDone);
                }

                if (swCounter % parsmc.saveConfigurationStreamInterval == 0) {
                    buffer_local_system_configuration();
                }

                // // This is a good time to write the current system configuration to disk
                // if (parsmc.saveConfigurationStreamText) {
                //     replica->saveConfigurationStreamText(
                //         control_parameter_subdir(local_current_parameter_index));
                // }
                // if (parsmc.saveConfigurationStreamBinary) {
                //     replica->saveConfigurationStreamBinary(
                //         control_parameter_subdir(local_current_parameter_index));
                // }
            }
            ++sweepsDone;
            if (swCounter == parsmc.saveInterval) {
                if (processIndex == 0) {
                    std::cout << "  " << sweepsDone << " ... saving results and state ...";
                }
                swCounter = 0;
                save();
                //MPI_Barrier(MPI_COMM_WORLD);
                world.barrier();
                if (processIndex == 0) {
                    std::cout << " OK" << std::endl;
                }
            }
            if (sweepsDone == parsmc.sweeps) {
                swCounter = 0;
                finishedStage();
            }
            break;  //case M
        }

        case F:
            break;  //case F

        }  //switch

        //replica exchange
        if (stage == T or stage == M) {
            if (parspt.exchangeInterval != 0 and ((sweepsDone + sweepsDoneThermalization) % parspt.exchangeInterval == 0)) {
                replicaExchangeStep();
            }
            replicaExchangeConsistencyCheck();
        } //replica exchange
        
    } // while (stage != F)
}



template<class Model, class ModelParams>
void DetQMCPT<Model, ModelParams>::replicaExchangeStep() {
    timing.start("detqmcpt-replicaExchangeStep");

    namespace mpi = boost::mpi;
    mpi::communicator world;
    
    // Gather control_data_buffer contents from all processes:
    local_control_data_buffer.clear();
    replica->get_control_data(local_control_data_buffer);
    if (processIndex == 0) {
        assert(process_control_data_buffer.size() == (std::size_t)numProcesses);
        for (auto& datastring : process_control_data_buffer) {
            datastring.clear();
        }
    }
    mpi::gather(world,
                local_control_data_buffer,   // send
                process_control_data_buffer, // recv at rank 0
                0);
                
    // double* local_buf = local_control_data_buffer.data();
    // uint32_t local_buf_size = local_control_data_buffer.size();
    // replica->get_control_data(local_buf);
    // MPI_Gather( local_buf,                        // send buf
    //             local_buf_size,
    //             MPI_DOUBLE,
    //             control_data_buffer_1.data(),     // recv buf
    //             local_buf_size,
    //             MPI_DOUBLE,
    //             0,                                // root process
    //             MPI_COMM_WORLD
    //     );
    
                
    // Gather exchange action contribution from replicas
    double localAction = replica->get_exchange_action_contribution();
    // MPI_Gather( &localAction,           // send buf
    //             1,
    //             MPI_DOUBLE,
    //             exchange_action.data(), // recv buf
    //             1,
    //             MPI_DOUBLE,
    //             0,                      // root process
    //             MPI_COMM_WORLD
    //     );
    mpi::gather(world,
                localAction,     // send
                exchange_action, // recv at rank 0
                0);


    if (processIndex == 0) {

        //update histograms of replicas moving up or down
        for (int pi = 0; pi < numProcesses; ++pi) {
            int nPar = current_process_par[pi];            
            if (nPar == numProcesses - 1) {
                es.process_goingWhere[pi] = ExchangeStatistics::DOWN_P;
            } else if (nPar == 0) {
                es.process_goingWhere[pi] = ExchangeStatistics::UP_P;
            }
            if (es.process_goingWhere[pi] == ExchangeStatistics::DOWN_P) {
                ++es.par_countGoingDown[nPar];
            } else if (es.process_goingWhere[pi] == ExchangeStatistics::UP_P) {
                ++es.par_countGoingUp[nPar];
            }
        }
        
        // serially walk through control parameters and propose exchange
        for (int cpi1 = 0; cpi1 < numProcesses - 1; ++cpi1) {
            int cpi2 = cpi1 + 1;
            double par1 = parspt.controlParameterValues[cpi1];
            double par2 = parspt.controlParameterValues[cpi2];
            int indexProc1 = current_par_process[cpi1];
            int indexProc2 = current_par_process[cpi2];
            double action1 = exchange_action[indexProc1];                    
            double action2 = exchange_action[indexProc2];

            num exchange_prob = get_replica_exchange_probability<Model>(par1, action1,
                                                                        par2, action2);
            ++es.par_swapUpProposed[cpi1];
            if (exchange_prob >= 1 or rng.rand01() <= exchange_prob) {
                ++es.par_swapUpAccepted[cpi1];
                // swap control parameters
                current_process_par[indexProc1] = cpi2;
                current_process_par[indexProc2] = cpi1;
                current_par_process[cpi1] = indexProc2;
                current_par_process[cpi2] = indexProc1;
                // swap control parameter data
                std::swap(process_control_data_buffer[indexProc1],
                          process_control_data_buffer[indexProc2]);
                
                // // take control parameter data in swapped process order
                // //  control_data_buffer_2 { indexProc1 } = control_data_buffer_1 { indexProc2 }
                // std::copy( control_data_buffer_1.begin() + indexProc2 * local_buf_size,       // input begin
                //            control_data_buffer_1.begin() + (indexProc2 + 1) * local_buf_size, // input end
                //            control_data_buffer_2.begin() + indexProc1 * local_buf_size        // output begin
                //     );
                // //  control_data_buffer_2 { indexProc2 } = control_data_buffer_1 { indexProc1 }
                // std::copy( control_data_buffer_1.begin() + indexProc1 * local_buf_size,       // input begin
                //            control_data_buffer_1.begin() + (indexProc1 + 1) * local_buf_size, // input end
                //            control_data_buffer_2.begin() + indexProc2 * local_buf_size        // output begin
                //     );
            } // else {
            //     // take control parameter data in original process order
            //     //  control_data_buffer_2 { indexProc1 } = control_data_buffer_1 { indexProc1 }
            //     std::copy( control_data_buffer_1.begin() + indexProc1 * local_buf_size,       // input begin
            //                control_data_buffer_1.begin() + (indexProc1 + 1) * local_buf_size, // input end
            //                control_data_buffer_2.begin() + indexProc1 * local_buf_size        // output begin
            //         );
            //     //  control_data_buffer_2 { indexProc2 } = control_data_buffer_1 { indexProc2 }
            //     std::copy( control_data_buffer_1.begin() + indexProc2 * local_buf_size,       // input begin
            //                control_data_buffer_1.begin() + (indexProc2 + 1) * local_buf_size, // input end
            //                control_data_buffer_2.begin() + indexProc2 * local_buf_size        // output begin
            //         );
            // }
        }
    } // if (processIndex == 0)
    // distribute and update control parameter
    int new_param_index = 0;
    // MPI_Scatter( current_process_par.data(), // send buf
    //              1,
    //              MPI_INT,
    //              &new_param_index,           // recv buf
    //              1,
    //              MPI_INT,
    //              0,                          // root process
    //              MPI_COMM_WORLD
    //     );
    mpi::scatter(world,
                 current_process_par, // send
                 new_param_index,     // recv
                 0);
    replica->set_exchange_parameter_value(
        parspt.controlParameterValues[new_param_index]
        );
    local_current_parameter_index = new_param_index;
    // distribute new control parameter data and update replica
    local_control_data_buffer.clear();
    mpi::scatter(world,
                 process_control_data_buffer, // send at rank 0
                 local_control_data_buffer,   // recv 
                 0);
    // MPI_Scatter( control_data_buffer_2.data(), // send buf
    //              local_buf_size,
    //              MPI_DOUBLE,
    //              local_buf,                    // recv buf
    //              local_buf_size,
    //              MPI_DOUBLE,
    //              0,                            // root process
    //              MPI_COMM_WORLD
    //     );
    replica->set_control_data(local_control_data_buffer);

    timing.stop("detqmcpt-replicaExchangeStep");
}


template<class Model, class ModelParams>
void DetQMCPT<Model, ModelParams>::replicaExchangeConsistencyCheck() {
    double local_exchange_parameter_value = replica->get_exchange_parameter_value();
    double diff = local_exchange_parameter_value -
        parspt.controlParameterValues[local_current_parameter_index];
    if ( std::abs(diff) > 1E-10 ) {
        throw_GeneralError("local_current_parameter_index mismatch!");
    }
    std::vector<double> process_par_values(numProcesses, 0.0);
    // MPI_Gather( &local_exchange_parameter_value, // send buf
    //             1,
    //             MPI_DOUBLE,
    //             process_par_values.data(), // recv buf
    //             1,
    //             MPI_DOUBLE,
    //             0,              // root process
    //             MPI_COMM_WORLD
    //     );
    namespace mpi = boost::mpi;
    mpi::communicator world;
    mpi::gather(world,
                local_exchange_parameter_value, // send
                process_par_values,             // recv
                0);    
    if (processIndex == 0) {
        for (int pi = 0; pi < numProcesses; ++pi) {
            num v1 = process_par_values[pi];
            num v2 = parspt.controlParameterValues[current_process_par[pi]];
            if ( std::abs(v1 - v2) > 1E-10 ) {
                throw_GeneralError("Exchange parameter value mismatch!");
            }
        }
    }
}




template<class Model, class ModelParams>
void DetQMCPT<Model, ModelParams>::saveResults() {
    timing.start("saveResults");

    outputResults(obsHandlers);
    for (auto p = obsHandlers.begin(); p != obsHandlers.end(); ++p) {
        (*p)->outputTimeseries();
    }
    outputResults(vecObsHandlers);

    timing.stop("saveResults");
}




#endif /* MPIDETQMCPT_H_ */
