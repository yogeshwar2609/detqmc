/*
 * detsdw.cpp
 *
 *  Created on: Feb 21, 2013
 *      Author: gerlach
 */

#include <cmath>
#include <numeric>
#include <functional>
#include <boost/assign/std/vector.hpp>    // 'operator+=()' for vectors
#include "observable.h"
#include "detsdw.h"
#include "exceptions.h"
#include "timing.h"

//initial values for field components chosen from this range:
const num PhiLow = -1;
const num PhiHigh = 1;
//adjustment of phiDelta:
const num InitialPhiDelta = 0.5;
const uint32_t AccRatioAdjustmentSamples = 100;
const num phiDeltaGrowFactor = 1.01;
const num phiDeltaShrinkFactor = 0.99;


std::unique_ptr<DetSDW> createDetSDW(RngWrapper& rng, ModelParams pars) {
	//TODO: add checks
	pars = updateTemperatureParameters(pars);

	//check parameters: passed all that are necessary
	using namespace boost::assign;
	std::vector<std::string> neededModelPars;
	neededModelPars += "mu", "L", "r", "accRatio";
	for (auto p = neededModelPars.cbegin(); p != neededModelPars.cend(); ++p) {
		if (pars.specified.count(*p) == 0) {
			throw ParameterMissing(*p);
		}
	}

	if (pars.checkerboard and pars.L % 2 != 0) {
		throw ParameterWrong("Checker board decomposition only supported for even linear lattice sizes");
	}

#define IF_NOT_POSITIVE(x) if (pars.specified.count(#x) > 0 and pars.x <= 0)
#define CHECK_POSITIVE(x) 	{  					  						\
								IF_NOT_POSITIVE(x) {  					\
									throw ParameterWrong(#x, pars.x);	\
								}										\
							}
	CHECK_POSITIVE(L);
#undef CHECK_POSITIVE
#undef IF_NOT_POSITIVE

	return std::unique_ptr<DetSDW>(new DetSDW(rng, pars));
}

DetSDW::DetSDW(RngWrapper& rng_, const ModelParams& pars) :
		DetModelGC<1,cpx>(pars, 4 * pars.L*pars.L),
		rng(rng_),
		checkerboard(pars.checkerboard),
		L(pars.L), N(L*L), r(pars.r), mu(pars.mu), c(1), u(1), lambda(1), //TODO: make these controllable by parameter
		hopHor(), hopVer(), sinhHopHor(), sinhHopVer(), coshHopHor(), coshHopVer(),
		spaceNeigh(L), timeNeigh(m),
		propK(), propKx(propK[XBAND]), propKy(propK[YBAND]),
		g(green[0]), gFwd(greenFwd[0]), gBwd(greenBwd[0]),
		phi0(N, m+1), phi1(N, m+1), phi2(N, m+1), phiCosh(N, m+1), phiSinh(N, m+1),
		phiDelta(InitialPhiDelta),
		targetAccRatio(pars.accRatio), lastAccRatio(0), accRatioRA(AccRatioAdjustmentSamples),
		normPhi(), sdwSusc(),
		kOcc(), kOccX(kOcc[XBAND]), kOccY(kOcc[YBAND]),
		kOccImag(), kOccXimag(kOccImag[XBAND]), kOccYimag(kOccImag[YBAND]),
		occ(), occX(occ[XBAND]), occY(occ[YBAND]),
		occImag(), occXimag(occImag[XBAND]), occYimag(occImag[YBAND])
{
	g = CubeCpx(4*N,4*N, m+1);
	if (pars.timedisplaced) {
		gFwd = CubeCpx(4*N,4*N, m+1);
		gBwd = CubeCpx(4*N,4*N, m+1);
	}
	setupRandomPhi();

	//hopping constants. These are the t_ij in sum_<i,j> -t_ij c^+_i c_j
	//So for actual calculations an additional minus-sign needs to be included.
	hopHor[XBAND] = -1.0;
	hopVer[XBAND] = -0.5;
	hopHor[YBAND] =  0.5;
	hopVer[YBAND] =  1.0;
	//precalculate hyperbolic functions, used in checkerboard decomposition
	using std::sinh; using std::cosh;
	for_each_band( [this](Band band) {
		sinhHopHor[band] = sinh(-dtau * hopHor[band]);
		coshHopHor[band] = cosh(-dtau * hopHor[band]);
		sinhHopVer[band] = sinh(-dtau * hopVer[band]);
		coshHopVer[band] = cosh(-dtau * hopVer[band]);
	} );

	setupPropK();
	computeBmat[0] = [this](uint32_t k2, uint32_t k1) {
		return this->computeBmatSDW(k2, k1);
	};
	if (checkerboard) {
		leftMultiplyBmat[0] = [this](const MatCpx& A, uint32_t k2, uint32_t k1) {
			return this->checkerboardLeftMultiplyBmat(A, k2, k1);
		};
		rightMultiplyBmat[0] = [this](const MatCpx& A, uint32_t k2, uint32_t k1) {
			return this->checkerboardRightMultiplyBmat(A, k2, k1);
		};
		leftMultiplyBmatInv[0] = [this](const MatCpx& A, uint32_t k2, uint32_t k1) {
			return this->checkerboardLeftMultiplyBmatInv(A, k2, k1);
		};
		rightMultiplyBmatInv[0] = [this](const MatCpx& A, uint32_t k2, uint32_t k1) {
			return this->checkerboardRightMultiplyBmatInv(A, k2, k1);
		};
	} else {
		leftMultiplyBmat[0] = [this](const MatCpx& A, uint32_t k2, uint32_t k1) {
			return this->computeBmatSDW(k2, k1) * A;
		};
		rightMultiplyBmat[0] = [this](const MatCpx& A, uint32_t k2, uint32_t k1) {
			return A * this->computeBmatSDW(k2, k1);
		};
		leftMultiplyBmatInv[0] = [this](const MatCpx& A, uint32_t k2, uint32_t k1) {
			return arma::inv(this->computeBmatSDW(k2, k1)) * A;
		};
		rightMultiplyBmatInv[0] = [this](const MatCpx& A, uint32_t k2, uint32_t k1) {
			return A * arma::inv(this->computeBmatSDW(k2, k1));
		};
	}

	setupUdVStorage();

	using std::cref;
	using namespace boost::assign;
	obsScalar += ScalarObservable(cref(normPhi), "normPhi", "np"),
			ScalarObservable(cref(sdwSusc), "sdwSusceptibility", "sdwsusc");

	kOccX.zeros(N);
	kOccY.zeros(N);
	obsVector += VectorObservable(cref(kOccX), N, "kOccX", "nkx"),
			VectorObservable(cref(kOccY), N, "kOccY", "nky");
	kOccXimag.zeros(N);
	kOccYimag.zeros(N);
	obsVector += VectorObservable(cref(kOccXimag), N, "kOccXimag", "nkximag"),
			VectorObservable(cref(kOccYimag), N, "kOccYimag", "nkyimag");

	occX.zeros(N);
	occY.zeros(N);
	obsVector += VectorObservable(cref(occX), N, "occX", "nx"),
			VectorObservable(cref(occY), N, "occY", "ny");
	occXimag.zeros(N);
	occYimag.zeros(N);
	obsVector += VectorObservable(cref(occXimag), N, "occXimag", "nximag"),
			VectorObservable(cref(occYimag), N, "occYimag", "nyimag");
}

DetSDW::~DetSDW() {
}

uint32_t DetSDW::getSystemN() const {
	return N;
}

MetadataMap DetSDW::prepareModelMetadataMap() const {
	MetadataMap meta;
	meta["model"] = "sdw";
	meta["checkerboard"] = (checkerboard ? "true" : "false");
	meta["timedisplaced"] = (timedisplaced ? "true" : "false");
#define META_INSERT(VAR) {meta[#VAR] = numToString(VAR);}
	META_INSERT(targetAccRatio);
	META_INSERT(r);
	META_INSERT(mu);
	META_INSERT(L);
	META_INSERT(d);
	META_INSERT(N);
	META_INSERT(beta);
	META_INSERT(m);
	META_INSERT(dtau);
	META_INSERT(s);
#undef META_INSERT
	return meta;
}

void DetSDW::measure() {
	timing.start("sdw-measure");
	Phi meanPhi;
	meanPhi[0] = averageWholeSystem(phi0, 0.0);
	meanPhi[1] = averageWholeSystem(phi1, 0.0);
	meanPhi[2] = averageWholeSystem(phi2, 0.0);
	normPhi = arma::norm(meanPhi, 2);

	//fermion occupation number -- real space
	//probably not very interesting data
	occX.zeros(N);
	occY.zeros(N);
	occXimag.zeros(N);
	occYimag.zeros(N);
#pragma omp parallel for
	for (uint32_t l = 1; l <= m; ++l) {
		for (uint32_t i = 0; i < N; ++i) {
			occX[i] += std::real(g.slice(l)(i, i) + g.slice(l)(i+N, i+N));
			occY[i] += std::real(g.slice(l)(i+2*N, i+2*N) + g.slice(l)(i+3*N, i+3*N));
			occXimag[i] += std::imag(g.slice(l)(i, i) + g.slice(l)(i+N, i+N));
			occYimag[i] += std::imag(g.slice(l)(i+2*N, i+2*N) + g.slice(l)(i+3*N, i+3*N));
		}
	}
	using std::ref;
	for (VecNum& occ : {ref(occX), ref(occY), ref(occXimag), ref(occYimag)}) {
		occ /= num(m) * num(N);
	}

	//fermion occupation number -- k-space
	kOccX.zeros(N);
	kOccY.zeros(N);
	kOccXimag.zeros(N);
	kOccYimag.zeros(N);
	static const num pi = M_PI;
#pragma omp parallel for
	for (uint32_t ksitey = 0; ksitey < L; ++ksitey) {			//k-vectors
		num ky = 2 * pi * num(ksitey) / num(L);
		for (uint32_t ksitex = 0; ksitex < L; ++ksitex) {
			num kx = 2 * pi * num(ksitex) / num(L);
			uint32_t ksite = L*ksitey + ksitex;
			for (uint32_t l = 1; l <= m; ++l) {					//timeslices
				for (uint32_t jy = 0; jy < L; ++jy) {			//sites j
					for (uint32_t jx = 0; jx < L; ++jx) {
						uint32_t j = L*jy + jx;
						for (uint32_t iy = 0; iy < L; ++iy) {	//sites i
							for (uint32_t ix = 0; ix < L; ++ix) {
								uint32_t i = L*iy + ix;
								cpx phase = std::exp(cpx(0, kx * (ix - jx) + ky * (iy - jy)));
								cpx diracDelta = cpx((i == j ? 1.0 : 0.0), 0);
								cpx greenEntryXBandSpinUp   = g.slice(l)(i, j);
								cpx greenEntryXBandSpinDown = g.slice(l)(i + N, j + N);
								cpx greenEntryYBandSpinUp   = g.slice(l)(i + 2*N, j + 2*N);
								cpx greenEntryYBandSpinDown = g.slice(l)(i + 3*N, j + 3*N);
								kOccX[ksite] += std::real(phase * ( diracDelta - greenEntryXBandSpinUp
																  + diracDelta - greenEntryXBandSpinDown));
								kOccY[ksite] += std::real(phase * ( diracDelta - greenEntryYBandSpinUp
																  + diracDelta - greenEntryYBandSpinDown));
								//imaginary parts should add up to zero..., but for now check:
								kOccXimag[ksite] += std::imag(phase * ( diracDelta - greenEntryXBandSpinUp
																  	  + diracDelta - greenEntryXBandSpinDown));
								kOccYimag[ksite] += std::imag(phase * ( diracDelta - greenEntryYBandSpinUp
																  	  + diracDelta - greenEntryYBandSpinDown));
							}
						}
					}
				}
			}
		}
	}
	using std::ref;
	for (VecNum& kocc : {ref(kOccX), ref(kOccY), ref(kOccXimag), ref(kOccYimag)}) {
		kocc /= num(m) * num(N);
	}

	//sdw-susceptibility
	sdwSusc = dtau * sumWholeSystem( [this](uint32_t site, uint32_t timeslice) {
											return phi0(site, timeslice) * phi0(0, m)
												 + phi1(site, timeslice) * phi1(0, m)
												 + phi2(site, timeslice) * phi2(0, m);
										},
									0.0);

	timing.stop("sdw-measure");
}

void DetSDW::setupRandomPhi() {
	for_each_timeslice( [this](uint32_t k) {
		for_each_site( [this, k](uint32_t site) {
			phi0(site, k) = rng.randRange(PhiLow, PhiHigh);
			phi1(site, k) = rng.randRange(PhiLow, PhiHigh);
			phi2(site, k) = rng.randRange(PhiLow, PhiHigh);
			num phiNorm = std::sqrt(std::pow(phi0(site, k), 2)
									+ std::pow(phi1(site, k), 2)
									+ std::pow(phi2(site, k), 2));
			phiCosh(site, k) = std::cosh(dtau * phiNorm);
			phiSinh(site, k) = std::sinh(dtau * phiNorm) / phiNorm;
		} );
	} );
}

void DetSDW::setupPropK() {
	checkarray<checkarray<num,z>, 2> t;
	t[XBAND][XPLUS] = t[XBAND][XMINUS] = hopHor[XBAND];
	t[XBAND][YPLUS] = t[XBAND][YMINUS] = hopVer[XBAND];
	t[YBAND][XPLUS] = t[YBAND][XMINUS] = hopHor[YBAND];
	t[YBAND][YPLUS] = t[YBAND][YMINUS] = hopVer[YBAND];

	for_each_band( [this, &t](uint32_t band) {
		MatNum k = -mu * arma::eye(N,N);
		for_each_site( [this, band, &k, &t](uint32_t site) {
			for (uint32_t dir = 0; dir < z; ++dir) {
				uint32_t neigh = spaceNeigh(dir, site);
				k(site, neigh) -= t[band][dir];					//Minus sign!
			}
		} );
//		std::string name = std::string("k") + (band == XBAND ? "x" : band == YBAND ? "y" : "error");
//		debugSaveMatrix(k, name);
		propK[band] = computePropagator(dtau, k);
//		debugSaveMatrix(propK[band], "prop" + name);
	} );
}


MatCpx DetSDW::computeBmatSDW(uint32_t k2, uint32_t k1) const {
	timing.start("computeBmatSDW_direct");
	using arma::eye; using arma::zeros; using arma::diagmat;
	if (k2 == k1) {
		return MatCpx(eye(4*N,4*N), zeros(4*N,4*N));
	}
	assert(k2 > k1);
	assert(k2 <= m);

	//compute the matrix e^(-dtau*V_k) * e^(-dtau*K)
	auto singleTimesliceProp = [this, N](uint32_t k) {
		timing.start("singleTimesliceProp_direct");
		MatCpx result(4*N, 4*N);

		//submatrix view helper for a 4N*4N matrix
		auto block = [&result, N](uint32_t row, uint32_t col) {
			return result.submat(row * N, col * N,
					             (row + 1) * N - 1, (col + 1) * N - 1);
		};
		auto& kphi0 = phi0.col(k);
		auto& kphi1 = phi1.col(k);
		auto& kphi2 = phi2.col(k);
//		debugSaveMatrix(kphi0, "kphi0");
//		debugSaveMatrix(kphi1, "kphi1");
//		debugSaveMatrix(kphi2, "kphi2");
		auto& kphiCosh = phiCosh.col(k);
		auto& kphiSinh = phiSinh.col(k);
		//TODO: is this the best way to set the real and imaginary parts of a complex submatrix?
		//TODO: compare to using set_real / set_imag
		block(0, 0) = MatCpx(diagmat(kphiCosh) * propKx,
				             zeros(N,N));
		block(0, 1).zeros();
		block(0, 2) = MatCpx(diagmat(-kphi2 % kphiSinh) * propKy,
				             zeros(N,N));
		block(0, 3) = MatCpx(diagmat(-kphi0 % kphiSinh) * propKy,
				             diagmat(+kphi1 % kphiSinh) * propKy);
		block(1, 0).zeros();
		block(1, 1) = block(0, 0);
		block(1, 2) = MatCpx(diagmat(-kphi0 % kphiSinh) * propKy,
				             diagmat(-kphi1 % kphiSinh) * propKy);
		block(1, 3) = MatCpx(diagmat(+kphi2 % kphiSinh) * propKy,
				             zeros(N,N));
		block(2, 0) = MatCpx(diagmat(-kphi2 % kphiSinh) * propKx,
				             zeros(N,N));
		block(2, 1) = MatCpx(diagmat(-kphi0 % kphiSinh) * propKx,
				             diagmat(+kphi1 % kphiSinh) * propKx);
		block(2, 2) = MatCpx(diagmat(kphiCosh) * propKy,
				             zeros(N,N));
		block(2, 3).zeros();
		block(3, 0) = MatCpx(diagmat(-kphi0 % kphiSinh) * propKx,
				             diagmat(-kphi1 % kphiSinh) * propKx);
		block(3, 1) = MatCpx(diagmat(+kphi2 % kphiSinh) * propKx,
				             zeros(N,N));
		block(3, 2).zeros();
		block(3, 3) = block(2, 2);

//		debugSaveMatrix(arma::real(result), "emdtauVemdtauK_real");
//		debugSaveMatrix(arma::imag(result), "emdtauVemdtauK_imag");
		timing.stop("singleTimesliceProp_direct");
		return result;
	};

	MatCpx result = singleTimesliceProp(k2);

	for (uint32_t k = k2 - 1; k > k1; --k) {
		result *= singleTimesliceProp(k);				// equivalent to: result = result * singleTimesliceProp(k);
	}

	timing.stop("computeBmatSDW_direct");

	return result;
}

// with sign = +/- 1, band = XBAND|YBAND: set R := E^(sign * dtau * K_band) * A
template <class Matrix> inline
MatCpx DetSDW::cbLMultHoppingExp(const Matrix& A, Band band, int sign) {
	MatCpx result = A;		//can't avoid this copy

	auto applyBondFactorsLeft = [this, band, &result](NeighDir neigh, num ch, num sh) {
		for (uint32_t i = 0; i < N; ++i) {
			uint32_t j = spaceNeigh(neigh, i);
			//change rows i and j of result
			for (uint32_t col = 0; col < N; ++col) {
				cpx new_icol = ch * result(i, col) + sh * result(j, col);
				cpx new_jcol = sh * result(i, col) + ch * result(j, col);
				result(i,col) = new_icol;
				result(j,col) = new_jcol;
			}
		}
	};

	//horizontal bonds
	applyBondFactorsLeft(XPLUS, coshHopHor[band], sign * sinhHopHor[band]);

	//vertical bonds
	applyBondFactorsLeft(YPLUS, coshHopVer[band], sign * sinhHopVer[band]);

	return result;
}



// with sign = +/- 1, band = XBAND|YBAND: return A * E^(sign * dtau * K_band)
template <class Matrix> inline
MatCpx DetSDW::cbRMultHoppingExp(const Matrix& A, Band band, int sign) {
	MatCpx result = A;		//can't avoid this copy

	auto applyBondFactorsRight = [this, band, &result](NeighDir neigh, num ch, num sh) {
		for (uint32_t i = 0; i < N; ++i) {
			uint32_t j = spaceNeigh(neigh, i);
			//change columns i and j of result
			for (uint32_t row = 0; row < N; ++row) {
				cpx new_rowi = ch * result(row, i) + sh * result(row, i);
				cpx new_rowj = sh * result(row, i) + ch * result(row, j);
				result(row,i) = new_rowi;
				result(row,j) = new_rowj;
			}
		}
	};

	//horizontal bonds
	applyBondFactorsRight(XPLUS, coshHopHor[band], sign * sinhHopHor[band]);

	//vertical bonds
	applyBondFactorsRight(YPLUS, coshHopVer[band], sign * sinhHopVer[band]);

	return result;
}




MatCpx DetSDW::checkerboardLeftMultiplyBmat(const MatCpx& A, uint32_t k2, uint32_t k1) {
	assert(k2 > k1);
	assert(k2 <= m);

	//helper: submatrix block for a matrix
	auto block = [N](const MatCpx& mat, uint32_t row, uint32_t col) {
		return mat.submat( row * N, col * N,
		                  (row + 1) * N - 1, (col + 1) * N - 1);
	};

	//helper: multiply B(k,k-1) from left to orig, return result
	auto leftMultiplyBk = [this, block](const MatCpx orig, uint32_t k) -> MatCpx {
		const auto& kphi0 = phi0.col(k);
		const auto& kphi1 = phi1.col(k);
		const auto& kphi2 = phi2.col(k);
		const auto& c = phiCosh.col(k);			// cosh(dtau * |phi|)
		const auto& kphiSinh = phiSinh.col(k);	// sinh(dtau * |phi|) / |phi|
		VecNum ax  =  kphi2 % kphiSinh;
		VecNum max = -kphi2 % kphiSinh;
		VecCpx bx  {kphi0, -kphi1};
		VecCpx bcx {kphi0, kphi1};

		MatCpx result(4*N, 4*N);

		for (uint32_t col = 0; col < 4; ++col) {
			using arma::diagmat;
			//only three terms each time because of zero blocks in the E^(-dtau*V) matrix
			block(result, 0, col) = diagmat(c)  * cbLMultHoppingExp(block(orig, 0, col), XBAND, -1)
								  + diagmat(ax) * cbLMultHoppingExp(block(orig, 2, col), YBAND, -1)
								  + diagmat(bx) * cbLMultHoppingExp(block(orig, 3, col), YBAND, -1);

			block(result, 1, col) = diagmat(c)   * cbLMultHoppingExp(block(orig, 1, col), XBAND, -1)
								  + diagmat(bcx) * cbLMultHoppingExp(block(orig, 2, col), YBAND, -1)
							      + diagmat(max) * cbLMultHoppingExp(block(orig, 3, col), YBAND, -1);

			block(result, 2, col) = diagmat(ax) * cbLMultHoppingExp(block(orig, 0, col), XBAND, -1)
							      + diagmat(bx) * cbLMultHoppingExp(block(orig, 1, col), XBAND, -1)
							      + diagmat(c)  * cbLMultHoppingExp(block(orig, 2, col), YBAND, -1);

			block(result, 3, col) = diagmat(bcx) * cbLMultHoppingExp(block(orig, 0, col), XBAND, -1)
							      + diagmat(max) * cbLMultHoppingExp(block(orig, 1, col), XBAND, -1)
							      + diagmat(c)   * cbLMultHoppingExp(block(orig, 3, col), YBAND, -1);
		}
		return result;
	};

	MatCpx result = leftMultiplyBk(A, k1 + 1);

	for (uint32_t k = k1 + 2; k <= k2; ++k) {
		result = leftMultiplyBk(result, k);
	}

	//chemical potential terms:
	result *= std::exp(+dtau * (k2 - k1) * mu);

	return result;
}


MatCpx DetSDW::checkerboardLeftMultiplyBmatInv(const MatCpx& A, uint32_t k2, uint32_t k1) {
	assert(k2 > k1);
	assert(k2 <= m);

	//helper: submatrix block for a matrix
	auto block = [N](const MatCpx& mat, uint32_t row, uint32_t col) {
		return mat.submat( row * N, col * N,
		                  (row + 1) * N - 1, (col + 1) * N - 1);
	};

	//helper: multiply B(k,k-1)^-1 from left to orig, return result
	auto leftMultiplyBkInv = [this, block](const MatCpx orig, uint32_t k) -> MatCpx {
		const auto& kphi0 = phi0.col(k);
		const auto& kphi1 = phi1.col(k);
		const auto& kphi2 = phi2.col(k);
		const auto& c = phiCosh.col(k);			// cosh(dtau * |phi|)
		const auto& kphiSinh = phiSinh.col(k);	// sinh(dtau * |phi|) / |phi|
		VecNum ax  =  kphi2 % kphiSinh;
		VecNum max = -kphi2 % kphiSinh;
		VecCpx mbx  {-kphi0, kphi1};
		VecCpx mbcx {kphi0, -kphi1};

		MatCpx result(4*N, 4*N);

		for (uint32_t col = 0; col < 4; ++col) {
			using arma::diagmat;
			//only three terms each time because of zero blocks in the E^(dtau*V) matrix
			block(result, 0, col) = cbLMultHoppingExp(diagmat(c)   * block(orig, 0, col), XBAND, +1)
								  + cbLMultHoppingExp(diagmat(max) * block(orig, 2, col), XBAND, +1)
								  + cbLMultHoppingExp(diagmat(mbx) * block(orig, 3, col), XBAND, +1);

			block(result, 1, col) = cbLMultHoppingExp(diagmat(c)    * block(orig, 1, col), XBAND, +1)
								  + cbLMultHoppingExp(diagmat(mbcx) * block(orig, 2, col), XBAND, +1)
							      + cbLMultHoppingExp(diagmat(ax)   * block(orig, 3, col), XBAND, +1);

			block(result, 2, col) = cbLMultHoppingExp(diagmat(max) * block(orig, 0, col), YBAND, +1)
							      + cbLMultHoppingExp(diagmat(mbx) * block(orig, 1, col), YBAND, +1)
							      + cbLMultHoppingExp(diagmat(c)   * block(orig, 2, col), YBAND, +1);

			block(result, 3, col) = cbLMultHoppingExp(diagmat(mbcx) * block(orig, 0, col), YBAND, +1)
							      + cbLMultHoppingExp(diagmat(ax)   * block(orig, 1, col), YBAND, +1)
							      + cbLMultHoppingExp(diagmat(c)    * block(orig, 3, col), YBAND, +1);
		}
		return result;
	};

	MatCpx result = leftMultiplyBkInv(A, k1 + 1);

	for (uint32_t k = k1 + 2; k <= k2; ++k) {
		result = leftMultiplyBkInv(result, k);
	}

	//chemical potential terms:
	result *= std::exp(-dtau * (k2 - k1) * mu);

	return result;
}

MatCpx DetSDW::checkerboardRightMultiplyBmat(const MatCpx& A, uint32_t k2, uint32_t k1) {
	assert(k2 > k1);
	assert(k2 <= m);

	//helper: submatrix block for a matrix
	auto block = [N](const MatCpx& mat, uint32_t row, uint32_t col) {
		return mat.submat( row * N, col * N,
		                  (row + 1) * N - 1, (col + 1) * N - 1);
	};

	//helper: multiply B(k,k-1) from right to orig, return result
	auto rightMultiplyBk = [this, block](const MatCpx orig, uint32_t k) -> MatCpx {
		const auto& kphi0 = phi0.col(k);
		const auto& kphi1 = phi1.col(k);
		const auto& kphi2 = phi2.col(k);
		const auto& c = phiCosh.col(k);			// cosh(dtau * |phi|)
		const auto& kphiSinh = phiSinh.col(k);	// sinh(dtau * |phi|) / |phi|
		VecNum ax  =  kphi2 % kphiSinh;
		VecNum max = -kphi2 % kphiSinh;
		VecCpx bx  {kphi0, -kphi1};
		VecCpx bcx {kphi0, kphi1};

		MatCpx result(4*N, 4*N);

		for (uint32_t row = 0; row < 4; ++row) {
			using arma::diagmat;
			//only three terms each time because of zero blocks in the E^(-dtau*V) matrix
			block(result, row, 0) = cbRMultHoppingExp(block(orig, row, 0) * diagmat(c),   XBAND, -1)
								  + cbRMultHoppingExp(block(orig, row, 2) * diagmat(ax),  XBAND, -1)
								  + cbRMultHoppingExp(block(orig, row, 3) * diagmat(bcx), XBAND, -1);

			block(result, row, 1) = cbRMultHoppingExp(block(orig, row, 1) * diagmat(c),   XBAND, -1)
								  + cbRMultHoppingExp(block(orig, row, 2) * diagmat(bx),  XBAND, -1)
							      + cbRMultHoppingExp(block(orig, row, 3) * diagmat(max), XBAND, -1);

			block(result, row, 2) = cbRMultHoppingExp(block(orig, row, 0) * diagmat(ax),  YBAND, -1)
							      + cbRMultHoppingExp(block(orig, row, 1) * diagmat(bcx), YBAND, -1)
							      + cbRMultHoppingExp(block(orig, row, 2) * diagmat(c),   YBAND, -1);

			block(result, row, 3) = cbRMultHoppingExp(block(orig, row, 0) * diagmat(bx),  YBAND, -1)
							      + cbRMultHoppingExp(block(orig, row, 1) * diagmat(max), YBAND, -1)
							      + cbRMultHoppingExp(block(orig, row, 3) * diagmat(c),   YBAND, -1);
		}
		return result;
	};

	MatCpx result = rightMultiplyBk(A, k2);

	for (uint32_t k = k2 - 1; k >= k1 +1; --k) {
		result = rightMultiplyBk(result, k);
	}

	//chemical potential terms:
	result *= std::exp(+dtau * (k2 - k1) * mu);

	return result;
}


MatCpx DetSDW::checkerboardRightMultiplyBmatInv(const MatCpx& A, uint32_t k2, uint32_t k1) {
	assert(k2 > k1);
	assert(k2 <= m);

	//helper: submatrix block for a matrix
	auto block = [N](const MatCpx& mat, uint32_t row, uint32_t col) {
		return mat.submat( row * N, col * N,
		                  (row + 1) * N - 1, (col + 1) * N - 1);
	};

	//helper: multiply B(k,k-1)^-1 from right to orig, return result
	auto rightMultiplyBkInv = [this, block](const MatCpx orig, uint32_t k) -> MatCpx {
		const auto& kphi0 = phi0.col(k);
		const auto& kphi1 = phi1.col(k);
		const auto& kphi2 = phi2.col(k);
		const auto& c = phiCosh.col(k);			// cosh(dtau * |phi|)
		const auto& kphiSinh = phiSinh.col(k);	// sinh(dtau * |phi|) / |phi|
		VecNum ax  =  kphi2 % kphiSinh;
		VecNum max = -kphi2 % kphiSinh;
		VecCpx mbx  {-kphi0, kphi1};
		VecCpx mbcx {kphi0, -kphi1};

		MatCpx result(4*N, 4*N);

		for (uint32_t row = 0; row < 4; ++row) {
			using arma::diagmat;
			//only three terms each time because of zero blocks in the E^(+dtau*V) matrix
			block(result, row, 0) = cbRMultHoppingExp(block(orig, row, 0), XBAND, +1) * diagmat(c)
								  + cbRMultHoppingExp(block(orig, row, 2), YBAND, +1) * diagmat(max)
								  + cbRMultHoppingExp(block(orig, row, 3), YBAND, +1) * diagmat(mbcx);

			block(result, row, 1) = cbRMultHoppingExp(block(orig, row, 1), XBAND, +1) * diagmat(c)
								  + cbRMultHoppingExp(block(orig, row, 2), YBAND, +1) * diagmat(mbx)
							      + cbRMultHoppingExp(block(orig, row, 3), YBAND, +1) * diagmat(ax);

			block(result, row, 2) = cbRMultHoppingExp(block(orig, row, 0), XBAND, +1) * diagmat(max)
							      + cbRMultHoppingExp(block(orig, row, 1), XBAND, +1) * diagmat(mbcx)
							      + cbRMultHoppingExp(block(orig, row, 2), YBAND, +1) * diagmat(c);

			block(result, row, 3) = cbRMultHoppingExp(block(orig, row, 0), XBAND, +1) * diagmat(mbx)
							      + cbRMultHoppingExp(block(orig, row, 1), XBAND, +1) * diagmat(ax)
							      + cbRMultHoppingExp(block(orig, row, 3), YBAND, +1) * diagmat(c);
		}
		return result;
	};

	MatCpx result = rightMultiplyBkInv(A, k2);

	for (uint32_t k = k2 - 1; k >= k1 +1; --k) {
		result = rightMultiplyBkInv(result, k);
	}

	//chemical potential terms:
	result *= std::exp(-dtau * (k2 - k1) * mu);

	return result;
}






void DetSDW::updateInSlice(uint32_t timeslice) {
	timing.start("sdw-updateInSlice");

	lastAccRatio = 0;
	for_each_site( [this, timeslice](uint32_t site) {
		Phi newphi = proposeNewField(site, timeslice);

//		VecNum oldphi0 = phi0.col(timeslice);
//		VecNum oldphi1 = phi1.col(timeslice);
//		VecNum oldphi2 = phi2.col(timeslice);
//		debugSaveMatrix(oldphi0, "old_phi0");
//		debugSaveMatrix(oldphi1, "old_phi1");
//		debugSaveMatrix(oldphi2, "old_phi2");

//		VecNum newphi0 = phi0.col(timeslice);
//		VecNum newphi1 = phi1.col(timeslice);
//		VecNum newphi2 = phi2.col(timeslice);
//		newphi0[site] = newphi[0];
//		newphi1[site] = newphi[1];
//		newphi2[site] = newphi[2];
//		debugSaveMatrix(newphi0, "new_phi0");
//		debugSaveMatrix(newphi1, "new_phi1");
//		debugSaveMatrix(newphi2, "new_phi2");

		num dsphi = deltaSPhi(site, timeslice, newphi);
		num propSPhi = std::exp(-dsphi);
//		std::cout << propSPhi << std::endl;

		//delta = e^(-dtau*V_new)*e^(+dtau*V_old) - 1

		//compute non-zero elements of delta
		//evMatrix(): yield a 4x4 matrix containing the entries for the
		//current lattice site and time slice of e^(sign*dtau*V) with
		//given values of the field phi at that space-time location [and of
		//cosh(dtau*|phi|) and sinh(dtau*|phi|) / |phi|]
		auto evMatrix = [](int sign, num kphi0, num kphi1,
						   num kphi2, num kphiCosh, num kphiSinh) -> MatCpx::fixed<4,4> {
			MatNum::fixed<4,4> ev_real;
			ev_real.diag().fill(kphiCosh);
			ev_real(0,1) = ev_real(1,0) = ev_real(2,3) = ev_real(3,2) = 0;
			ev_real(2,0) = ev_real(0,2) =  sign * kphi2 * kphiSinh;
			ev_real(2,1) = ev_real(0,3) =  sign * kphi0 * kphiSinh;
			ev_real(3,0) = ev_real(1,2) =  sign * kphi0 * kphiSinh;
			ev_real(3,1) = ev_real(1,3) = -sign * kphi2 * kphiSinh;

			MatCpx::fixed<4,4> ev;
			ev.set_real(ev_real);
			ev(0,3).imag(-sign * kphi1 * kphiSinh);
			ev(1,2).imag( sign * kphi1 * kphiSinh);
			ev(2,1).imag(-sign * kphi1 * kphiSinh);
			ev(3,0).imag( sign * kphi1 * kphiSinh);

			return ev;
		};
		MatCpx::fixed<4,4> evOld = evMatrix(
				+1,
				phi0(site, timeslice), phi1(site, timeslice), phi2(site, timeslice),
				phiCosh(site, timeslice), phiSinh(site, timeslice)
				);
		num normnewphi = arma::norm(newphi,2);
		num coshnewphi = std::cosh(dtau * normnewphi);
		num sinhnewphi = std::sinh(dtau * normnewphi) / normnewphi;
		MatCpx::fixed<4,4> emvNew = evMatrix(
				-1,
				newphi[0], newphi[1], newphi[2],
				coshnewphi, sinhnewphi
				);
		MatCpx::fixed<4,4> deltanonzero = emvNew * evOld;
		deltanonzero.diag() -= cpx(1.0, 0);

		//****
		//Compute the determinant and inverse of I + Delta*(I - G)
		//based on Sherman-Morrison formula / Matrix-Determinant lemma
		//****

		//Delta*(I - G) is a sparse matrix containing just 4 rows:
		//site, site+N, site+2N, site+3N
		//Compute the values of these rows [O(N)]:
		checkarray<VecCpx, 4> rows = {{VecCpx(4*N), VecCpx(4*N), VecCpx(4*N), VecCpx(4*N)}};
#pragma omp parallel for
		for (uint32_t r = 0; r < 4; ++r) {
			for (uint32_t col = 0; col < 4*N; ++col) {
				rows[r][col] = -deltanonzero(r,0) * g.slice(timeslice).col(col)[site];
			}
			rows[r][site] += deltanonzero(r,0);
			for (uint32_t dc = 1; dc < 4; ++dc) {
				for (uint32_t col = 0; col < 4*N; ++col) {
					rows[r][col] += -deltanonzero(r,dc) * g.slice(timeslice).col(col)[site + dc*N];
				}
				rows[r][site + dc*N] += deltanonzero(r,dc);
			}
		}

		// [I + Delta*(I - G)]^(-1) again is a sparse matrix
		// with four rows site, site+N, site+2N, site+3N
		// compute them iteratively, together with the determinant of
		// I + Delta*(I - G)
		// Apart from these rows, the remaining diagonal entries of
		// [I + Delta*(I - G)]^(-1) are 1
		//
		// before this loop rows[] holds the entries of Delta*(I - G),
		// after the loop rows[] holds the corresponding rows of [I + Delta*(I - G)]^(-1)
		cpx det = 1;
		for (uint32_t l = 0; l < 4; ++l) {
			VecCpx row = rows[l];
			for (int k = l-1; k >= 0; --k) {
				row[site + k*N] = 0;
			}
			for (int k = l-1; k >= 0; --k) {
				row += rows[l][site + k*N] * rows[k];
			}
			cpx divisor = cpx(1.0, 0) + row[site + l*N];
			rows[l] = (-1.0/divisor) * row;
			rows[l][site + l*N] += 1;
			for (int k = l - 1; k >= 0; --k) {
				rows[k] -= (rows[k][site + l*N] / divisor) * row;
			}
			det *= divisor;
		}

		//****DEBUG
//		checkarray<VecCpx, 4> invRows = {{rows[0], rows[1], rows[2], rows[3]}};
		//****END-DEBUG

		//****
		//DEBUG: This slow code was working before.
		//Compare results with the better-performing sherman-morrison code
//		SpMatCpx delta(4*N, 4*N);
//		arma::uvec::fixed<4> idx = {site, site + N, site + 2*N, site + 3*N};
//		//Armadilo lacks non-contiguous submatrix views for sparse matrices
//		uint32_t i = 0;
//		for (auto col: idx) {
//			uint32_t j = 0;
//			for (auto row: idx) {
//				delta(row, col) = deltanonzero(j, i);
//				++j;
//			}
//			++i;
//		}


//		MatCpx deltaDense(4*N,4*N);
//		deltaDense = delta;
//		debugSaveMatrix(MatNum(arma::real(deltaDense)), "delta_real");
//		debugSaveMatrix(MatNum(arma::imag(deltaDense)), "delta_imag");

//		//inefficient!
//		static MatCpx eyeCpx = MatCpx(arma::eye(4*N, 4*N), arma::zeros(4*N, 4*N));
//		MatCpx target = eyeCpx + delta * (eyeCpx - g.slice(timeslice));

//		debugSaveMatrix(MatNum(arma::real(target)), "target_real");
//		debugSaveMatrix(MatNum(arma::imag(target)), "target_imag");

//		cpx weightRatio = arma::det(target);
//		std::cout << weightRatio << std::endl;

//		std::cout << weightRatio << " vs. " << det << std::endl;
		//END DEBUG
		//****

		//****
		// DEBUG-CHECK Delta*(I - G) vs. rows
//		MatCpx check = delta * (eyeCpx - g.slice(timeslice));
//		for (uint32_t r = 0; r < 4; ++r) {
//			check.row(site + r*N) -= rows[r].st();
//		}
//		std::cout << "Row check: " << arma::max(arma::max(arma::abs(check))) << std::endl;
//		debugSaveMatrix(MatNum(arma::real(check)), "check_real");
//		debugSaveMatrix(MatNum(arma::imag(check)), "check_imag");
//		exit(0);
		// END-DEBUG-CHECK
		//****

		//****
		//DEBUG-CHECK [I+Delta*(I - G)]^(-1) vs. invRows
//		MatCpx inv = arma::inv(target);
//		inv -= eyeCpx;
//		for (uint32_t r = 0; r < 4; ++r) {
//			inv.row(site + r*N) -= invRows[r].st();
//			inv.row(site + r*N)[site + r*N] += 1;
//		}
//		std::cout << "inv-div: " << arma::max(arma::max(inv)) << std::endl;
		//END-DEBUG-CHECK
		//****

		num propSFermion = det.real();

		num prop = propSPhi * propSFermion;

		if (prop > 1.0 or rng.rand01() < prop) {
			//count accepted update
			lastAccRatio += 1.0;

//			num phisBefore = phiAction();
			phi0(site, timeslice) = newphi[0];
			phi1(site, timeslice) = newphi[1];
			phi2(site, timeslice) = newphi[2];
			phiCosh(site, timeslice) = coshnewphi;
			phiSinh(site, timeslice) = sinhnewphi;
//			num phisAfter = phiAction();
//			std::cout << std::scientific << dsphi << " vs. " << phisAfter << " - " << phisBefore << " = " <<
//					(phisAfter - phisBefore) << std::endl;

//			debugSaveMatrix(MatNum(arma::real(g.slice(timeslice))), "gslice_old_real");
//			debugSaveMatrix(MatNum(arma::imag(g.slice(timeslice))), "gslice_old_imag");
//			g.slice(timeslice) *= arma::inv(target);
//			debugSaveMatrix(MatNum(arma::real(g.slice(timeslice))), "gslice_new_real");
//			debugSaveMatrix(MatNum(arma::imag(g.slice(timeslice))), "gslice_new_imag");

			//****
			//DEBUG
//			MatCpx gPrimeRef = g.slice(timeslice) * arma::inv(target);
			//END DEBUG
			//****

			//compensate for already included diagonal entries of I in invRows
			rows[0][site] -= 1;
			rows[1][site + N] -= 1;
			rows[2][site + 2*N] -= 1;
			rows[3][site + 3*N] -= 1;
			//compute G' = G * [I + Delta*(I - G)]^(-1) = G * [I + invRows]
			MatCpx gTimesInvRows(4*N, 4*N);
			const auto& G = g.slice(timeslice);
#pragma omp parallel for
			for (uint32_t col = 0; col < 4*N; ++col) {
				for (uint32_t row = 0; row < 4*N; ++row) {
					gTimesInvRows(row, col) = G(row, site) * rows[0][col]
					                        + G(row, site + N) * rows[1][col]
					                        + G(row, site + 2*N) * rows[2][col]
					                        + G(row, site + 3*N) * rows[3][col]
					                        ;
				}
			}
			g.slice(timeslice) += gTimesInvRows;

			//****
			//DEBUG
//			std::cout << arma::max(arma::max(
//					(arma::abs(gPrimeRef - g.slice(timeslice))))) << std::endl;
			//END DEBUG
			//****
		}
	});
	lastAccRatio /= num(N);

	timing.stop("sdw-updateInSlice");
}

void DetSDW::updateInSliceThermalization(uint32_t timeslice) {
	updateInSlice(timeslice);

	accRatioRA.addValue(lastAccRatio);
	if (accRatioRA.getSamplesAdded() % AccRatioAdjustmentSamples == 0) {
		num avgAccRatio = accRatioRA.get();
		if (avgAccRatio < targetAccRatio) {
			phiDelta *= phiDeltaShrinkFactor;
		} else if (avgAccRatio > targetAccRatio) {
			phiDelta *= phiDeltaGrowFactor;
		}
	}
}





DetSDW::Phi DetSDW::proposeNewField(uint32_t site, uint32_t timeslice) {
	(void) site; (void) timeslice;
	//TODO: make this smarter!

	Phi phi = { phi0(site, timeslice),
			    phi1(site, timeslice),
			    phi2(site, timeslice)
	          };

//	static int comp = 0;
//	num r = rng.randRange(-PhiDelta, +PhiDelta);
//	phi[comp] += r;
//	comp = (comp + 1) % 3;

	for (auto& comp: phi) {
		num r = rng.randRange(-phiDelta, +phiDelta);
		comp += r;
	}

	return phi;
}

num DetSDW::deltaSPhi(uint32_t site, uint32_t timeslice, const Phi newphi) {
	//switched to asymmetric numerical derivative
	using arma::dot;

	const Phi oldphi = {phi0(site, timeslice),
					    phi1(site, timeslice),
					    phi2(site, timeslice)};
	Phi phiDiff = newphi - oldphi;

	num oldphiSq = dot(oldphi, oldphi);
	num newphiSq = dot(newphi, newphi);
	num phiSqDiff = newphiSq - oldphiSq;

	num oldphiPow4 = oldphiSq * oldphiSq;
	num newphiPow4 = newphiSq * newphiSq;
	num phiPow4Diff = newphiPow4 - oldphiPow4;

	uint32_t kEarlier = timeNeigh(ChainDir::MINUS, timeslice);
	Phi phiEarlier = Phi{phi0(site, kEarlier),
					     phi1(site, kEarlier),
					     phi2(site, kEarlier)};
	uint32_t kLater = timeNeigh(ChainDir::PLUS, timeslice);
	Phi phiLater = Phi{phi0(site, kLater),
					   phi1(site, kLater),
					   phi2(site, kLater)};
	Phi phiTimeNeigh = phiLater + phiEarlier;

	Phi phiSpaceNeigh = std::accumulate(
			spaceNeigh.beginNeighbors(site),
			spaceNeigh.endNeighbors(site),
			Phi{0,0,0},
			[this, timeslice] (Phi accum, uint32_t neighSite) {
				return accum + Phi{phi0(neighSite, timeslice),
					phi1(neighSite, timeslice),
					phi2(neighSite, timeslice)};
			}
	);


	num delta1 = (1.0 / (c * c * dtau)) * (phiSqDiff - dot(phiTimeNeigh, phiDiff));

	num delta2 = 0.5 * dtau * (z * phiSqDiff - 2.0 * dot(phiSpaceNeigh, phiDiff));

	num delta3 = dtau * (0.5 * r * phiSqDiff + 0.25 * u * phiPow4Diff);

	return delta1 + delta2 + delta3;
}


num DetSDW::phiAction() {
	//switched to asymmetric numerical derivative
	arma::field<Phi> phi(N, m+1);
	for (uint32_t timeslice = 1; timeslice <= m; ++timeslice) {
		for (uint32_t site = 0; site < N; ++site) {
			phi(site, timeslice)[0] = phi0(site, timeslice);
			phi(site, timeslice)[1] = phi1(site, timeslice);
			phi(site, timeslice)[2] = phi2(site, timeslice);
		}
	}
	num action = 0;
	for (uint32_t timeslice = 1; timeslice <= m; ++timeslice) {
		for (uint32_t site = 0; site < N; ++site) {
			Phi timeDerivative =
					(phi(site, timeslice) - phi(site, timeNeigh(ChainDir::MINUS, timeslice)))
					/ dtau;
			action += (dtau / (2.0 * c * c)) * arma::dot(timeDerivative, timeDerivative);

			//count only neighbors in PLUS-directions: no global overcounting of bonds
			Phi xneighDiff = phi(site, timeslice) -
					phi(spaceNeigh(XPLUS, site), timeslice);
			action += 0.5 * dtau * arma::dot(xneighDiff, xneighDiff);
			Phi yneighDiff = phi(site, timeslice) -
					phi(spaceNeigh(YPLUS, site), timeslice);
			action += 0.5 * dtau * arma::dot(yneighDiff, yneighDiff);

			num phisq = arma::dot(phi(site, timeslice), phi(site, timeslice));
			action += 0.5 * dtau * r * phisq;

			action += 0.25 * dtau * u * std::pow(phisq, 2);
		}
	}
	return action;
}


void DetSDW::thermalizationOver() {
	std::cout << "After thermalization: phiDelta = " << phiDelta << '\n'
			  << "lastAccRatio = " << lastAccRatio
			  << std::endl;
}
