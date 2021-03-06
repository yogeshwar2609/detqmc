/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  See the enclosed file LICENSE for a copy or if
 * that was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2017 Max H. Gerlach
 * 
 * */

/*
 * dataserieswriter.h
 *
 *  Created on: Jan 28, 2011
 *      Author: gerlach
 */

// recovered for SDW DQMC (2015-02-07 - )
//   -- note that dataserieswritersucc.h has nicer code

#ifndef DATASERIESWRITER_H_
#define DATASERIESWRITER_H_

#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
//#include <iomanip>

#include "metadata.h"

/*
 * write a container (providing an iterator) containing data to an ascii file
 * supports a header of the form:
 *
 * ## blah blah
 * ## blah
 * # foo1 = bar
 * # foo2 = 124
 * # foo4 = 28.983
 * ## palaver palaver
 * value0
 * value1
 * ...
 * etc.
 */
template <class Container>
class DataSeriesWriter {
public:
    void setData(const Container* dataSeries);

    template <class ValueType>
    void addMeta(const std::string& key, ValueType val);
    void addMetadataMap(const MetadataMap& meta);

    void addHeaderText(const std::string& headerText);

    void writeToFile(const std::string& filename);
    void writeToFile(const std::string& filename, unsigned floatPrecision);
private:
    const Container* data;
    std::string header;
};



template <class Container>
void DataSeriesWriter<Container>::setData(const Container* dataSeries) {
    data = dataSeries;
}

template <class Container>
template <class ValueType>
void DataSeriesWriter<Container>::addMeta(const std::string& key, ValueType val) {
    std::stringstream ss;
    ss << "# " << key << " = " << val << '\n';
    header += ss.str();
}

template <class Container>
void DataSeriesWriter<Container>::addMetadataMap(const MetadataMap& meta) {
    header += metadataToString(meta, "#");
}

template <class Container>
void DataSeriesWriter<Container>::addHeaderText(const std::string& headerText) {
    std::stringstream ss(headerText);
    std::string line;
    while (std::getline(ss, line)) {
        header += "## " + line + '\n';
    }
}

template <class Container>
void DataSeriesWriter<Container>::writeToFile(const std::string& filename) {
    std::ofstream output(filename.c_str());
    output << header;
    for (typename Container::const_iterator iter = data->begin(); iter != data->end(); ++iter) {
        output << *iter << '\n';
    }
}


template <class Container>
void DataSeriesWriter<Container>::writeToFile(const std::string& filename, unsigned floatPrecision) {
    std::ofstream output(filename.c_str());
    output.precision(floatPrecision);
    output.setf(std::ios::scientific, std::ios::floatfield);
    output << header;
    for (typename Container::const_iterator iter = data->begin(); iter != data->end(); ++iter) {
        output << *iter << '\n';
    }
}

typedef DataSeriesWriter<std::vector<double> > DoubleVectorWriter;
typedef DataSeriesWriter<std::vector<int> > IntVectorWriter;



#endif /* DATASERIESWRITER_H_ */
