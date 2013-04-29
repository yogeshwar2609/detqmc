/*
 * deteval.cpp
 *
 *  Created on: Apr 29, 2013
 *      Author: gerlach
 */

// Evaluate time series generated by detqmc.
// Call in directory containing timeseries files.

#include <iostream>
#include <memory>
#include <map>
#include <cmath>
#include <vector>
#include <string>
#include "boost/program_options.hpp"
#include "git-revision.h"
#include "tools.h"						//glob
#include "dataseriesloader.h"
#include "datamapwriter.h"
#include "metadata.h"
#include "exceptions.h"
#include "statistics.h"

int main(int argc, char **argv) {
	unsigned discard = 0;
	unsigned subsample = 1;
	unsigned jkBlocks = 1;

	//parse command line options
	namespace po = boost::program_options;
	po::options_description evalOptions("Time series evaluation options");
	evalOptions.add_options()
			("help", "print help on allowed options and exit")
			("version,v", "print version information (git hash, build date) and exit")
			("discard,d", po::value<unsigned>(&discard)->default_value(0),
					"number of initial time series entries to discard (additional thermalization)")
			("subsample,s", po::value<unsigned>(&subsample)->default_value(1),
					"take only every s'th sample into account")
			("jkblocks,j", po::value<unsigned>(&jkBlocks)->default_value(1),
					"number of jackknife blocks to use")
			;
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, evalOptions), vm);
	po::notify(vm);
	bool earlyExit = false;
	if (vm.count("help")) {
		std::cout << "Evaluate time series generated by detqmc.  Call in directory containing timeseries files.\n"
				  << "Will write results to files eval-results.dat and eval-tauint.dat\n\n"
				  << evalOptions << std::endl;
		earlyExit = true;
	}
	if (vm.count("version")) {
		std::cout << "Build info:\n"
				  << metadataToString(collectVersionInfo())
				  << std::endl;
		earlyExit = true;
	}
	if (earlyExit) {
		return 0;
	}

	//take simulation metadata from file info.dat
	MetadataMap meta = readOnlyMetadata("info.dat");
	unsigned guessedLength = static_cast<unsigned>(fromString<double>(meta.at("sweeps")) /
			fromString<double>(meta.at("measureInterval")));

	//Store averages / nonlinear estimates, jackknife errors,
	//integrated autocorrelation times here
	//key: observable name
	typedef std::map<std::string, double> ObsValMap;
	ObsValMap estimates, errors, tauints;
	//jackknife-block wise estimates
	typedef std::map<std::string, std::vector<double>> ObsVecMap;
	ObsVecMap jkBlockEstimates;

	//process time series files
	std::vector<std::string> filenames = glob("*.series");
	for (std::string fn : filenames) {
		std::cout << "Processing " << fn << ", ";
		DoubleSeriesLoader reader;
		reader.readFromFile(fn, subsample, discard, guessedLength);
		if (reader.getColumns() != 1) {
			throw GeneralError("File " + fn + " does not have exactly 1 column");
		}

		std::vector<double>* data = reader.getData();		//TODO: smart pointers!
		std::string obsName;
		reader.getMeta("observable", obsName);		//TODO: change class to yield return values, not output parameters
		std::cout << "observable: " << obsName << "..." << std::flush;

		estimates[obsName] = average(*data);
		jkBlockEstimates[obsName] = jackknifeBlockEstimates(*data, jkBlocks);

		if (obsName == "normPhi") {
			using std::pow;
			estimates["normPhiSquared"] = average<double>( [](double v) { return pow(v, 2); }, *data);
			jkBlockEstimates["normPhiSquared"] = jackknifeBlockEstimates<double>(
					[](double v) { return pow(v, 2); },
					*data, jkBlocks );
			estimates["normPhiFourth"] = average<double>( [](double v) { return pow(v, 4); }, *data);
			jkBlockEstimates["normPhiFourth"] = jackknifeBlockEstimates<double>(
					[](double v) { return pow(v, 4); },
					*data, jkBlocks );
			estimates["normPhiBinder"] = 1.0 - (3.0*estimates["normPhiFourth"]) /
											   (5.0*pow(estimates["normPhiSquared"], 2));
			jkBlockEstimates["normPhiBinder"] = std::vector<double>(jkBlocks, 0);
			for (unsigned jb = 0; jb < jkBlocks; ++jb) {
				jkBlockEstimates["normPhiBinder"][jb] =
						1.0 - (3.0*jkBlockEstimates["normPhiFourth"][jb]) /
						      (5.0*pow(jkBlockEstimates["normPhiSquared"][jb], 2));
			}
		}

		tauints[obsName] = tauint(*data);

		reader.deleteData();

		std::cout << std::endl;
	}

	//calculate error bars from jackknife block estimates
	for (auto const& nameBlockPair : jkBlockEstimates) {
		const std::string obsName = nameBlockPair.first;
		const std::vector<double> blockEstimates = nameBlockPair.second;
		errors[obsName] = jackknife(blockEstimates, estimates[obsName]);
	}

	StringDoubleMapWriter resultsWriter;
	resultsWriter.addMetadataMap(meta);
	resultsWriter.addMeta("eval-jackknife-blocks", jkBlocks);
	resultsWriter.addMeta("eval-discard", discard);
	resultsWriter.addMeta("eval-subsample", subsample);
	resultsWriter.addHeaderText("Averages and jackknife error bars computed from time series");
	resultsWriter.setData(std::make_shared<ObsValMap>(estimates));
	resultsWriter.setErrors(std::make_shared<ObsValMap>(errors));
	resultsWriter.writeToFile("eval-results.dat");

	StringDoubleMapWriter tauintWriter;
	tauintWriter.addMetadataMap(meta);
	tauintWriter.addMeta("eval-discard", discard);
	tauintWriter.addMeta("eval-subsample", subsample);
	tauintWriter.addHeaderText("Tauint estimates computed from time series");
	tauintWriter.setData(std::make_shared<ObsValMap>(tauints));
	tauintWriter.writeToFile("eval-tauint.dat");

	std::cout << "Done!" << std::endl;

	return 0;
}
