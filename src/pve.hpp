//
// Created by kerin on 2019-01-08.
//

#ifndef BGEN_PROG_PVE_HPP
#define BGEN_PROG_PVE_HPP

#include "genotype_matrix.hpp"
#include "file_utils.hpp"
#include "parameters.hpp"
#include "data.hpp"
#include "eigen_utils.hpp"

#include <boost/iostreams/filtering_stream.hpp>

#include <random>

namespace boost_io = boost::iostreams;

struct Index_t {
	Index_t() : main(0), noise(1) {
	}
	long main, gxe, noise;
};

Eigen::MatrixXd project_out_covars(Eigen::Ref<Eigen::MatrixXd> rhs,
                                   Eigen::Ref<Eigen::MatrixXd> C,
                                   const Eigen::Ref<const Eigen::MatrixXd>& CtC_inv,
                                   const parameters& p){
	assert(CtC_inv.cols() == C.cols());
	assert(CtC_inv.rows() == C.cols());
	assert(C.rows() == rhs.rows());
	if(p.mode_debug) std::cout << "Starting project_out_covars" << std::endl;
	Eigen::MatrixXd beta = CtC_inv * C.transpose() * rhs;
	Eigen::MatrixXd yhat = C * beta;
	Eigen::MatrixXd res = rhs - yhat;
	if(p.mode_debug) std::cout << "Ending project_out_covars" << std::endl;
	return res;
}

class PVE_Component {
public:
	Eigen::MatrixXd _XXtz, _XXtWz;
	double ytXXty;
	std::string label;
	long n_env;
	long n_covar;
	long n_samples;
	long n_draws;
	long n_jacknife_local;
	long rm_jacknife_block;
	double n_var_local;
	parameters params;
	bool is_active;

	// Storage for jacknife blocks
	std::vector<Eigen::MatrixXd> _XXtzs, _XXtWzs;
	std::vector<double> n_vars_local;
	std::vector<double> ytXXtys;

	Eigen::MatrixXd& C, CtC_inv, zz, Wzz;
	Eigen::VectorXd Y;
	Eigen::VectorXd eta;
	PVE_Component(const parameters& myparams,
	              Eigen::VectorXd& myY,
	              Eigen::MatrixXd& myzz,
	              Eigen::MatrixXd& myWzz,
	              Eigen::MatrixXd& myC,
	              Eigen::MatrixXd& myCtC_inv,
	              const long& myNJacknifeLocal) : params(myparams), Y(myY),
		zz(myzz), Wzz(myWzz), C(myC), CtC_inv(myCtC_inv), n_jacknife_local(myNJacknifeLocal) {
		assert(n_jacknife_local > 0);
		n_covar = C.cols();
		n_samples = zz.rows();
		n_draws = zz.cols();

		n_env = 0;
		label = "";
		is_active = true;
		rm_jacknife_block = -1;

		ytXXty = 0;
		ytXXtys.resize(n_jacknife_local, 0);

		n_var_local = 0;
		n_vars_local.resize(n_jacknife_local, 0);

		_XXtzs.resize(n_jacknife_local);
		_XXtWzs.resize(n_jacknife_local);
		for(long ii = 0; ii < n_jacknife_local; ii++) {
			Eigen::MatrixXd mm = Eigen::MatrixXd::Zero(n_samples, n_draws);
			_XXtzs[ii] = mm;
			_XXtWzs[ii] = mm;
		}

	}

	void set_eta(Eigen::Ref<Eigen::VectorXd> myeta){
		assert(is_active);
		n_env = 1;
		eta = myeta;
		Y.array() *= eta.array();
		zz.array().colwise() *= eta.array();
		Wzz.array().colwise() *= eta.array();
	}

	void set_inactive(){
		// The inactive component corresponds to sigma_e
		// Ie the 'noise' component
		assert(n_env == 0);
		is_active = false;
		_XXtz = zz;
		_XXtWz = Wzz;
		n_var_local = 1;
		ytXXty = Y.squaredNorm();
	}

	void add_to_trace_estimator(Eigen::Ref<Eigen::MatrixXd> X,
	                            long jacknife_index = 0){
		assert(jacknife_index < n_jacknife_local);
		if(is_active) {
			ytXXtys[jacknife_index] += (X.transpose() * Y).squaredNorm();
			Eigen::MatrixXd Xtz = X.transpose() * zz;
			_XXtzs[jacknife_index] += X * Xtz;
			if(n_covar > 0) {
				Eigen::MatrixXd XtWz = X.transpose() * Wzz;
				_XXtWzs[jacknife_index] += X * XtWz;
			}
			n_vars_local[jacknife_index] += X.cols();
		}
	}

	void finalise(){
		// Sum over the different jacknife blocks;
		if(is_active) {
			if(n_env > 0) {
				for (auto& mm : _XXtzs) {
					mm.array().colwise() *= eta.array();
				}
				for (auto& mm : _XXtWzs) {
					mm.array().colwise() *= eta.array();
				}
			}

			_XXtz = Eigen::MatrixXd::Zero(n_samples, n_draws);
			_XXtWz = Eigen::MatrixXd::Zero(n_samples, n_draws);
			for (auto& mm : _XXtzs) {
				_XXtz += mm;
			}
			for (auto& mm : _XXtWzs) {
				_XXtWz += mm;
			}

			n_var_local = std::accumulate(n_vars_local.begin(), n_vars_local.end(), 0.0);
			ytXXty = std::accumulate(ytXXtys.begin(), ytXXtys.end(), 0.0);
		}
	}

	Eigen::MatrixXd getXXtz() const {
		if(rm_jacknife_block >= 0) {
			return (_XXtz - _XXtzs[rm_jacknife_block]);
		} else {
			return _XXtz;
		}
	}

	Eigen::MatrixXd getXXtWz() const {
		if(rm_jacknife_block >= 0) {
			return (_XXtWz - _XXtWzs[rm_jacknife_block]);
		} else {
			return _XXtWz;
		}
	}

	double get_bb_trace() const {
		if(rm_jacknife_block >= 0) {
			return (ytXXty - ytXXtys[rm_jacknife_block]) / get_n_var_local();
		} else {
			return ytXXty / get_n_var_local();
		}
	}

	double get_n_var_local() const {
		if(rm_jacknife_block >= 0) {
			return n_var_local - n_vars_local[rm_jacknife_block];
		} else {
			return n_var_local;
		}
	}

	double operator*(const PVE_Component& other) const {
		double res;
		if(n_covar == 0) {
			res = getXXtz().cwiseProduct(other.getXXtz()).sum();
		} else if (n_covar > 0) {
			if(label == "noise" || other.label == "noise") {
				res = getXXtz().cwiseProduct(other.getXXtWz()).sum();
			} else {
				Eigen::MatrixXd XXtz = getXXtz();
				Eigen::MatrixXd WXXtz = project_out_covars(XXtz);
				res = WXXtz.cwiseProduct(other.getXXtWz()).sum();
			}
		} else {
			throw std::runtime_error("Error in PVE_Component");
		}
		return res / get_n_var_local() / other.get_n_var_local() / (double) n_draws;
	}

	Eigen::MatrixXd project_out_covars(Eigen::Ref<Eigen::MatrixXd> rhs) const {
		return ::project_out_covars(rhs, C, CtC_inv, params);
	}
};

class PVE {
public:
	// constants
	long n_draws;
	long n_samples;
	long n_components;
	long n_covar;
	long n_env;
	long n_var;
	double N;

	const parameters& p;

	const GenotypeMatrix& X;

	Eigen::VectorXd eta;
	Eigen::VectorXd Y;
	Eigen::MatrixXd& C;
	Eigen::MatrixXd CtC_inv;
	Eigen::ArrayXd sigmas, sigmasb;
	Eigen::ArrayXXd sigmas_jack, h2_jack, h2b_jack;
	Eigen::ArrayXd h2, h2_se_jack, h2_bias_corrected;
	Eigen::ArrayXd h2b, h2b_se_jack, h2b_bias_corrected;
	Eigen::ArrayXd n_var_jack;

	EigenDataMatrix zz;
	EigenDataMatrix Wzz;
	const std::unordered_map<long, bool>& sample_is_invalid;
	Data& data;

	// std::vector<std::string> components;
	std::vector<PVE_Component> components;
	Index_t ind;

	PVE(Data& dat,
	    Eigen::VectorXd& myY,
	    Eigen::MatrixXd& myC,
	    Eigen::VectorXd& myeta) : p(dat.p), X(dat.G), eta(myeta), Y(myY), C(myC),
		sample_is_invalid(dat.sample_is_invalid), data(dat) {
		n_samples = data.n_samples;
		N = n_samples;
		n_draws = p.n_pve_samples;

		n_covar = C.cols();
		n_env = 1;
	}

	PVE(Data& dat,
	    Eigen::VectorXd& myY,
	    Eigen::MatrixXd& myC) : p(dat.p), X(dat.G), Y(myY), C(myC),
		sample_is_invalid(dat.sample_is_invalid), data(dat) {
		n_samples = data.n_samples;
		N = n_samples;
		n_draws = p.n_pve_samples;

		n_covar = C.cols();
		n_env = 0;
	}

	void initialise_components(){
		zz.resize(n_samples, n_draws);
		if(p.rhe_random_vectors_file != "NULL") {
			EigenUtils::read_matrix(p.rhe_random_vectors_file, zz);
		} else {
			fill_gaussian_noise(p.random_seed, zz, n_samples, n_draws);
		}

		std::cout << "Initialising HE-regression components with:" << std::endl;
		std::cout << " - N-jacknife = " << p.n_jacknife << std::endl;
		std::cout << " - N-draws = " << p.n_pve_samples << std::endl;
		std::cout << " - N-samples = " << n_samples << std::endl;
		std::cout << " - N-covars = " << n_covar << std::endl;

		if(n_covar > 0) {
			Wzz = project_out_covars(zz);
			Y = project_out_covars(Y);
		} else {
			Wzz = zz;
		}

		// Set variance components
		if(true) {
			PVE_Component comp(p, Y, zz, Wzz, C, CtC_inv, p.n_jacknife);
			comp.label = "G";
			components.push_back(comp);
		}

		if(n_env == 1) {
			PVE_Component comp(p, Y, zz, Wzz, C, CtC_inv, p.n_jacknife);
			comp.label = "GxE";
			comp.set_eta(eta);
			components.push_back(comp);

			ind.main = 0;
			ind.gxe = 1;
			ind.noise = 2;
		} else {
			ind.main = 0;
			ind.noise = 1;
		}

		if(true) {
			PVE_Component comp(p, Y, zz, Wzz, C, CtC_inv, p.n_jacknife);
			comp.set_inactive();
			comp.label = "noise";
			components.push_back(comp);
		}
		n_components = components.size();

		std::cout << " - N-components = " << n_components - 1 << std::endl;
#ifndef OSX
		std::cout << "Initialised with ";
		std::cout << (double) fileUtils::getValueRAM() / 1000 / 1000;
		std::cout << "GB of RAM" << std::endl;
#endif
	}

	void calc_sigmas_v2(){
		// Compute randomised traces
		if(p.bgen_file != "NULL") {
			n_var = data.n_var;
			long n_main_segs;
			n_main_segs = (data.n_var + p.main_chunk_size - 1) / p.main_chunk_size;
			std::vector< std::vector <long> > main_fwd_pass_chunks(n_main_segs);
			for(long kk = 0; kk < data.n_var; kk++) {
				long main_ch_index = kk / p.main_chunk_size;
				main_fwd_pass_chunks[main_ch_index].push_back(kk);
			}

			EigenDataMatrix D;
			long jknf_block_size = (X.cumulative_pos[data.n_var - 1] + p.n_jacknife - 1) / p.n_jacknife;
			for (auto& iter_chunk : main_fwd_pass_chunks) {
				if(D.cols() != iter_chunk.size()) {
					D.resize(n_samples, iter_chunk.size());
				}
				X.col_block3(iter_chunk, D);

				// Get jacknife block (just use block assignment of 1st snp)
				long jacknife_index = X.cumulative_pos[iter_chunk[0]] / jknf_block_size;

				for (auto& comp : components) {
					comp.add_to_trace_estimator(D, jacknife_index);
				}
			}
		} else if (p.streamBgenFile != "NULL") {
			n_var = 0;
			Eigen::MatrixXd D;
			bool bgen_pass = true;
			long n_var_parsed = 0;
			long ch = 0;
			long print_interval = 100;
			if(p.mode_debug) print_interval = 1;
			long jack_block_size = (data.streamBgenView->number_of_variants() + p.n_jacknife) / p.n_jacknife;
			while (fileUtils::read_bgen_chunk(data.streamBgenView, D, sample_is_invalid,
			                                  n_samples, 128, p, bgen_pass, n_var_parsed)) {
				n_var += D.cols();
				if (ch % print_interval == 0 && ch > 0) {
					std::cout << "Chunk " << ch << " read (size " << 128;
					std::cout << ", " << n_var_parsed - 1 << "/" << data.streamBgenView->number_of_variants();
					std::cout << " variants parsed)" << std::endl;
				}

				// Get jacknife block (just use block assignment of 1st snp)
				long jacknife_index = n_var_parsed / jack_block_size;

				long n_chunk = D.cols();
				std::vector<std::string> placeholder(n_chunk, "col");
				EigenUtils::center_matrix(D);
				EigenUtils::scale_matrix_and_remove_constant_cols(D, n_chunk, placeholder);

				for (auto &comp : components) {
					comp.add_to_trace_estimator(D, jacknife_index);
				}
				ch++;
			}
			if(p.verbose) std::cout << n_var << " variants pass QC filters" << std::endl;
		}
		for (long ii = 0; ii < n_components; ii++) {
			components[ii].finalise();
		}

		// Solve system to estimate sigmas
		long n_components = components.size();
		for (long ii = 0; ii < n_components; ii++) {
			components[ii].rm_jacknife_block = -1;
		}
		Eigen::MatrixXd CC = construct_vc_system(components);
		Eigen::MatrixXd A = CC.block(0, 0, n_components, n_components);
		Eigen::VectorXd bb = CC.col(n_components);

		std::cout << "A: " << std::endl << A << std::endl;
		std::cout << "b: " << std::endl << bb << std::endl;
		sigmas = A.colPivHouseholderQr().solve(bb);
		h2 = calc_h2(A, bb, false);
		h2b = calc_h2(A, bb, true);

		boost_io::filtering_ostream outf;
		if(p.mode_debug) {
			auto filename = fileUtils::fstream_init(outf, p.out_file, "", "_rhe_debug");
			std::cout << "Writing RHE debugging info to " << filename << std::endl;
			Eigen::VectorXd tmp(Eigen::Map<Eigen::VectorXd>(CC.data(),CC.cols()*CC.rows()));
			outf << -1 << " " << tmp.transpose() << std::endl;
		}

		// jacknife estimates
		std::cout << "Computing standard errors using " << p.n_jacknife << " jacknife blocks" << std::endl;
		sigmas_jack.resize(p.n_jacknife, n_components);
		h2_jack.resize(p.n_jacknife, n_components);
		h2b_jack.resize(p.n_jacknife, n_components);
		n_var_jack.resize(p.n_jacknife);

		for (long jj = 0; jj < p.n_jacknife; jj++) {
			for (long ii = 0; ii < n_components; ii++) {
				components[ii].rm_jacknife_block = jj;
			}
			n_var_jack[jj] = components[0].get_n_var_local();

			Eigen::MatrixXd CC = construct_vc_system(components);
			Eigen::MatrixXd AA = CC.block(0, 0, n_components, n_components);
			Eigen::VectorXd bb = CC.col(n_components);
			Eigen::VectorXd ss = AA.colPivHouseholderQr().solve(bb);
			sigmas_jack.row(jj) = ss;
			h2b_jack.row(jj) = calc_h2(AA, bb, true);
			h2_jack.row(jj) = calc_h2(AA, bb, false);

			if(p.mode_debug) {
				Eigen::VectorXd tmp(Eigen::Map<Eigen::VectorXd>(CC.data(),CC.cols()*CC.rows()));
				outf << jj << " " << tmp.transpose() << std::endl;
			}
		}
		for (long ii = 0; ii < n_components; ii++) {
			components[ii].rm_jacknife_block = -1;
		}
		if(p.mode_debug) {
			boost_io::close(outf);
		}

		if(n_env > 0) {
			// Main effects model
			Eigen::MatrixXd A1(2, 2);
			Eigen::VectorXd bb1(2);
			A1(0, 0) = A(ind.main, ind.main);
			A1(0, 1) = A(ind.main, ind.noise);
			A1(1, 0) = A(ind.noise, ind.main);
			A1(1, 1) = A(ind.noise, ind.noise);

			bb1 << bb(ind.main), bb(ind.noise);
			Eigen::VectorXd sigmas1 = A1.colPivHouseholderQr().solve(bb1);
			Eigen::VectorXd h2_1 = sigmas1 / sigmas1.sum();
			std::cout << "h2-G = " << h2_1(0, 0) << " (main effects model only)" << std::endl;
		}
	}

	Eigen::MatrixXd construct_vc_system(const std::vector<PVE_Component>& components){
		Eigen::MatrixXd res(n_components, n_components + 1);
		for (long ii = 0; ii < n_components; ii++) {
			res(ii, n_components) = components[ii].get_bb_trace();
			for (long jj = 0; jj <= ii; jj++) {
				if(ii == jj && components[ii].label == "noise") {
					res(ii, jj) = n_samples - n_covar;
				} else {
					res(ii, jj) = components[ii] * components[jj];
					res(jj, ii) = res(ii, jj);
				}
			}
		}
		return res;
	}

	Eigen::ArrayXd calc_h2(Eigen::Ref<Eigen::MatrixXd> AA,
	                       Eigen::Ref<Eigen::VectorXd> bb,
	                       const bool& reweight_sigmas = false){
		Eigen::ArrayXd ss = AA.colPivHouseholderQr().solve(bb);
		if(reweight_sigmas) {
			ss *= (AA.row(AA.rows()-1)).array() / n_samples;
		}
		return ss / ss.sum();
	}

	void calc_h2(){
		// Rescale h2 to avoid bias
		for (long ii = 0; ii < n_components - 1; ii++) {
			h2_jack.col(ii) *= n_var / n_var_jack;
			h2b_jack.col(ii) *= n_var / n_var_jack;
		}

		// SE of h2
		h2_se_jack.resize(n_components);
		h2b_se_jack.resize(n_components);
		for (long ii = 0; ii < n_components; ii++) {
			h2_se_jack[ii] = std::sqrt(get_jacknife_var(h2_jack.col(ii)));
			h2b_se_jack[ii] = std::sqrt(get_jacknife_var(h2b_jack.col(ii)));
		}

		// bias correction
		h2_bias_corrected.resize(n_components);
		h2b_bias_corrected.resize(n_components);
		for (long ii = 0; ii < n_components; ii++) {
			h2_bias_corrected[ii] = get_jacknife_bias_correct(h2_jack.col(ii), h2(ii));
			h2b_bias_corrected[ii] = get_jacknife_bias_correct(h2b_jack.col(ii), h2b(ii));
		}
	}

	double get_jacknife_var(Eigen::ArrayXd jack_estimates){
		double jack_var = (jack_estimates - jack_estimates.mean()).square().sum();
		jack_var *= (p.n_jacknife - 1.0) / p.n_jacknife;
		return jack_var;
	}

	double get_jacknife_bias_correct(Eigen::ArrayXd jack_estimates, double full_data_est){
		double res = p.n_jacknife * full_data_est - (p.n_jacknife - 1.0) * jack_estimates.mean();
		return res;
	}

	Eigen::MatrixXd project_out_covars(Eigen::Ref<Eigen::MatrixXd> rhs){
		if(n_covar > 0) {
			if (CtC_inv.rows() != n_covar) {
				if(p.mode_debug) std::cout << "Starting compute of CtC_inv" << std::endl;
				CtC_inv = (C.transpose() * C).inverse();
				if(p.mode_debug) std::cout << "Ending compute of CtC_inv" << std::endl;
			}
			return ::project_out_covars(rhs, C, CtC_inv, p);
		} else {
			return rhs;
		}
	}

	void run();

	void fill_gaussian_noise(unsigned int seed,
	                         Eigen::Ref<Eigen::MatrixXd> zz,
	                         long nn,
	                         long pp);

	void to_file(const std::string& file){
		boost_io::filtering_ostream outf;
		std::string suffix = "";
		if(p.mode_vb || p.mode_calc_snpstats) {
			suffix = "_pve";
		}
		auto filename = fileUtils::fstream_init(outf, file, "", suffix);

		std::cout << "Writing PVE results to " << filename << std::endl;
		outf << "component sigmas h2 h2_se h2_bias_corrected" << std::endl;

		for (int ii = 0; ii < n_components; ii++) {
			outf << components[ii].label << " ";
			outf << sigmas[ii] << " ";
			outf << h2[ii] << " ";
			outf << h2_se_jack[ii] << " ";
			outf << h2_bias_corrected[ii] << std::endl;
		}

		for (int ii = 0; ii < n_components; ii++) {
			outf << components[ii].label << "_v2 ";
			outf << sigmas[ii] << " ";
			outf << h2b[ii] << " ";
			outf << h2b_se_jack[ii] << " ";
			outf << h2b_bias_corrected[ii] << std::endl;
		}
		boost_io::close(outf);

		if(p.xtra_verbose) {
			auto filename = fileUtils::fstream_init(outf, file, "", suffix + "_jacknife");
			std::cout << "Writing jacknife estimates to " << filename << std::endl;

			outf << "n_jack";
			for (long ii = 0; ii < n_components; ii++) {
				outf << " " << components[ii].label;
			}
			outf << std::endl;
			for (long jj = 0; jj < p.n_jacknife; jj++) {
				outf << components[0].n_vars_local[jj];
				for (long ii = 0; ii < n_components; ii++) {
					outf << " " << h2_jack(jj, ii);
				}
				outf << std::endl;
			}
			boost_io::close(outf);
		}

		if(p.xtra_verbose) {
			auto filename = fileUtils::fstream_init(outf, file, "", suffix + "_jacknife_scaled");
			std::cout << "Writing rescaled jacknife estimates to " << filename << std::endl;

			outf << "n_jack";
			for (long ii = 0; ii < n_components; ii++) {
				outf << " " << components[ii].label;
			}
			outf << std::endl;
			for (long jj = 0; jj < p.n_jacknife; jj++) {
				outf << components[0].n_vars_local[jj];
				for (long ii = 0; ii < n_components; ii++) {
					outf << " " << h2b_jack(jj, ii);
				}
				outf << std::endl;
			}
			boost_io::close(outf);
		}
	}
};

void PVE::fill_gaussian_noise(unsigned int seed, Eigen::Ref<Eigen::MatrixXd> zz, long nn, long pp) {
	assert(zz.rows() == nn);
	assert(zz.cols() == pp);

	std::mt19937 generator{seed};
	std::normal_distribution<scalarData> noise_normal(0.0, 1);

	for (int bb = 0; bb < pp; bb++) {
		for (std::size_t ii = 0; ii < nn; ii++) {
			zz(ii, bb) = noise_normal(generator);
		}
	}
}

void PVE::run() {
	// Add intercept to covariates
	Eigen::MatrixXd ones = Eigen::MatrixXd::Constant(n_samples, 1, 1.0);
	if(n_covar > 0) {
		Eigen::MatrixXd C1(n_samples, n_covar + 1);
		C1 << C, ones;
		C = C1;
	} else {
		C = ones;
	}
	n_covar = C.cols();

	// Center and scale eta
	if(n_env > 0) {
		std::vector<std::string> placeholder = {"eta"};
		EigenUtils::center_matrix(eta);
		EigenUtils::scale_matrix_and_remove_constant_cols(eta, n_env, placeholder);
	}

	// Compute variance components
	initialise_components();
	if(n_env > 0) {
		std::cout << "G+GxE effects model (gaussian prior)" << std::endl;
		calc_sigmas_v2();
	} else {
		std::cout << "Main effects model (gaussian prior)" << std::endl;
		calc_sigmas_v2();
	}

	std::cout << "Variance components estimates" << std::endl;
	std::cout << sigmas << std::endl;

	calc_h2();
	std::cout << "PVE estimates" << std::endl;
	std::cout << h2 << std::endl;
}

#endif //BGEN_PROG_PVE_HPP
