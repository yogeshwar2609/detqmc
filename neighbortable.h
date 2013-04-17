/*
 * neighbortable.h
 *
 *  Created on: Feb 19, 2013
 *      Author: gerlach
 */

#ifndef NEIGHBORTABLE_H_
#define NEIGHBORTABLE_H_

#include <cmath>
#include <cassert>
#include <vector>
#include <utility>
#pragma GCC diagnostic ignored "-Weffc++"
#pragma GCC diagnostic ignored "-Wconversion"
#include <armadillo>
#pragma GCC diagnostic warning "-Weffc++"
#pragma GCC diagnostic warning "-Wconversion"

#include "tools.h"

#include "boost_serialize_armadillo.h"


//lattice directions are indexed as in +x,-x,+y,-y,+z,-z, ...
//enums provided for d <= 3, but code valid also for higher dimensions
enum NeighDir {
	XPLUS = 0, XMINUS = 1, YPLUS = 2, YMINUS = 3, ZPLUS = 4, ZMINUS = 5
};

typedef arma::Mat<unsigned> tableSites;


class PeriodicCubicLatticeNearestNeighbors {
public:
	PeriodicCubicLatticeNearestNeighbors(unsigned d_, unsigned L_) :
		d(d_), L(L_), N(static_cast<unsigned>(uint_pow(L,d))), z(2*d),
		nearestNeighbors(z, N)
	{
		using std::floor;
		std::vector<unsigned> curCoords(d);     //holds the x, y, z coordinate components of the current site
		std::vector<unsigned> newCoords(d);     //newly calculated coords of the neighbor
		for (unsigned site = 0; site < N; ++site) {
			unsigned reducedSite = site;
			for (int idim = int(d) - 1; idim >= 0; --idim) {
				unsigned dim = unsigned(idim);
				curCoords[dim] = unsigned(floor(reducedSite / uint_pow(L, dim)));
				reducedSite -= curCoords[dim] * uint_pow(L, dim);
			}
			assert(reducedSite == 0);
			for (unsigned dim = 0; dim < d; ++dim) {
				//neighbor in + direction, periodic
				newCoords = curCoords;
				newCoords[dim] = (newCoords[dim] + 1) % L;
				nearestNeighbors(dim * 2, site) = coordsToSite(newCoords);
				//neighbor in - direction, periodic
				newCoords = curCoords;
				newCoords[dim] = (newCoords[dim] - 1 + L) % L;
				nearestNeighbors(dim * 2 + 1, site) = coordsToSite(newCoords);
			}
		}
	}
	virtual ~PeriodicCubicLatticeNearestNeighbors() {
	}

	//get site index of nearest neighbor of site in NeighDir latticeDirection
	unsigned operator()(unsigned latticeDirection, unsigned site) const {
		assert(latticeDirection < z);
		assert(site < N);
		return nearestNeighbors(latticeDirection, site);
	}
	unsigned operator()(NeighDir latticeDirection, unsigned site) const {
		return operator()((unsigned) latticeDirection, site);
	}

	//iterators over the nearest neighbors of a site:
	auto beginNeighbors(unsigned site) -> tableSites::const_col_iterator const {
		return nearestNeighbors.begin_col(site);
	}

	auto endNeighbors(unsigned site) -> tableSites::const_col_iterator const {
		return nearestNeighbors.end_col(site);
	}


	unsigned coordsToSite(const std::vector<unsigned>& coords) const {
		assert(coords.size() == d);
		unsigned site = 0;
		for (unsigned dim = 0; dim < d; ++dim) {
			site += coords[dim] * uint_pow(L, dim);
		}
		return site;
	}
protected:
	unsigned d; 	//spatial dimension
	unsigned L;		//linear extent
	unsigned N;		//number of sites
	unsigned z;		//lattice coordination number
	//Neighbor table: columns index sites, rows index lattice directions
	tableSites nearestNeighbors;

protected:
	friend class boost::serialization::access;
    template<class Archive>
    void serialize(Archive& ar, const unsigned int version) {
    	ar & d & L & N & z;
    	ar & nearestNeighbors;
    }
};


class PeriodicSquareLatticeNearestNeighbors : public PeriodicCubicLatticeNearestNeighbors {
public:
	PeriodicSquareLatticeNearestNeighbors(unsigned L)
		: PeriodicCubicLatticeNearestNeighbors(2, L)
	{
		//nearestNeighbors.save("neighbors.csv", arma::csv_ascii);
	}
};


enum class ChainDir : unsigned {
	PLUS = 0, MINUS = 1
};


template <unsigned startWith=0>		//start the indexing with 0 or maybe 1 or something else...
class PeriodicChainNearestNeighbors : public PeriodicCubicLatticeNearestNeighbors {
public:
	PeriodicChainNearestNeighbors(unsigned L)
		: PeriodicCubicLatticeNearestNeighbors(1, L)
	{
		//nearestNeighbors.save("timeneighbors.csv", arma::csv_ascii);
	}

	unsigned operator()(unsigned latticeDirection, unsigned site) const {
		return PeriodicCubicLatticeNearestNeighbors::operator()(latticeDirection, site - startWith)
		       + startWith;
	}
	unsigned operator()(NeighDir latticeDirection, unsigned site) const {
		return PeriodicChainNearestNeighbors::operator()((unsigned) latticeDirection, site);
	}
	unsigned operator()(ChainDir latticeDirection, unsigned site) const {
		return PeriodicChainNearestNeighbors::operator()((unsigned) latticeDirection, site);
	}
	//iterators over the nearest neighbors of a site:
	auto beginNeighbors(unsigned site) -> tableSites::const_col_iterator const {
		return PeriodicCubicLatticeNearestNeighbors::beginNeighbors(site - startWith) + startWith;
	}
	auto endNeighbors(unsigned site) -> tableSites::const_col_iterator const {
		return PeriodicCubicLatticeNearestNeighbors::endNeighbors(site - startWith) + startWith;
	}
};



#endif /* NEIGHBORTABLE_H_ */
