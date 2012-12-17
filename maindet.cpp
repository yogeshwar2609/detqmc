/*
 * maindet.cpp
 *
 *  Created on: Dec 10, 2012
 *      Author: gerlach
 */


#include <boost/program_options.hpp>
#include <boost/version.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <tuple>
#include "git-revision.h"
#include "metadata.h"
#include "detqmc.h"


//Parse command line and configuration file to configure the parameters of our simulation.
//In case of invocation with --help or --version, only print some info.
//return a tuple (runSimulation = true or false, simulationParameterStruct)
std::tuple<bool,ModelParams,MCParams> configureSimulation(int argc, char **argv) {
	bool runSimulation = true;
	ModelParams modelpar;
	MCParams mcpar;

	//parse options
	namespace po = boost::program_options;
	using std::string;
	string confFileName;

	po::options_description genericOptions("Generic options, command line only");
	genericOptions.add_options()
			("version,v", "print version information (git hash, build date) and exit")
			("help", "print help on allowed options and exit")
			("conf,c", po::value<string>(&confFileName)->default_value("simulation.conf"),
					"specify configuration file to be used; settings in there will be overridden by command line arguments")
			;

	po::options_description modelOptions("Model parameters, specify via command line or config file");
	modelOptions.add_options()
			("model", po::value<string>(&modelpar.model)->default_value("hubbard"), "model to be simulated")
			("t", po::value<num>(&modelpar.t), "hopping energy")
			("U", po::value<num>(&modelpar.U), "potential energy")
			("mu", po::value<num>(&modelpar.mu), "chemical potential")
			("L", po::value<unsigned>(&modelpar.L), "linear spatial extent")
			("d", po::value<unsigned>(&modelpar.d), "spatial dimension")
			("beta", po::value<num>(&modelpar.beta), "inverse temperature (in units of 1/t, kB=1)")
			("m", po::value<unsigned>(&modelpar.m), "number of imaginary time discretization levels (beta = m*dtau)")
			;

	po::options_description mcOptions("Parameters for Monte Carlo simulation, specify via command line or config file");
	mcpar.saveInterval = 0;
	mcOptions.add_options()
			("sweeps", po::value<unsigned>(&mcpar.sweeps), "number of sweeps used for measurements")
			("thermalization", po::value<unsigned>(&mcpar.thermalization), "number of warm-up sweeps")
			("jkBlocks", po::value<unsigned>(&mcpar.jkBlocks)->default_value(1), "number of jackknife blocks for error estimation")
			("timeseries", po::bool_switch(&mcpar.timeseries)->default_value(false), "if specified, write time series of individual measurements to disk")
			("measureInterval", po::value<unsigned>(&mcpar.measureInterval)->default_value(1), "take measurements every [arg] sweeps")
			("saveInterval", po::value<unsigned>(&mcpar.saveInterval), "write measurements to disk every [arg] sweeps; default: only at end of simulation")
			;

	po::variables_map vm;

	//parse command line
	po::options_description cmdlineOptions;
	cmdlineOptions.add(genericOptions).add(modelOptions).add(mcOptions);
	po::store(po::parse_command_line(argc, argv, cmdlineOptions), vm);

	po::notify(vm);

	//parse config file, options specified there have lower precedence
	po::options_description confFileOptions;
	confFileOptions.add(modelOptions).add(mcOptions);
	std::ifstream ifsConf(confFileName);
	po::store(po::parse_config_file(ifsConf, confFileOptions), vm);

	po::notify(vm);

	using std::cout; using std::endl;
	if (vm.count("help")) {
		cout << "Usage:" << endl << endl
			 << genericOptions << endl
			 << modelOptions << endl
			 << mcOptions << endl;
		runSimulation = false;
	}
	if (vm.count("version")) {
		cout << metadataToString(collectVersionInfo());
		runSimulation = false;
	}

	//record which options have been specified
	auto record = [vm](const po::options_description& optDesc,
			std::set<std::string>& set) {
		auto opts = optDesc.options();
		for (auto p = opts.begin(); p != opts.end(); ++p) {
			const std::string& o = (*p)->long_name();
			if (vm.count(o)) {
				set.insert(o);
			}
		}
	};
	record(modelOptions, modelpar.specified);
	record(mcOptions, mcpar.specified);

	return std::make_tuple(runSimulation, modelpar, mcpar);
}


int main(int argc, char **argv) {
	ModelParams parmodel;
	MCParams parmc;
	bool runSimulation;
	std::tie(runSimulation, parmodel, parmc) = configureSimulation(argc, argv);

	if (runSimulation) {
		DetQMC simulation(parmodel, parmc);
		simulation.run();
	}

	return 0;
}