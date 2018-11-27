/* re-implementation of variational bayes algorithm for 1D GxE

How to use Eigen::Ref:
https://stackoverflow.com/questions/21132538/correct-usage-of-the-eigenref-class

Building static executable:
https://stackoverflow.com/questions/3283021/compile-a-standalone-static-executable
*/
#ifndef VBAYES_X2_HPP
#define VBAYES_X2_HPP

#include <algorithm>
#include <chrono>     // start/end time info
#include <ctime>      // start/end time info
#include <cstdint>    // uint32_t
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <thread>
#include <set>
#include "sys/types.h"
#include "class.h"
#include "data.hpp"
#include "my_timer.hpp"
#include "utils.hpp"
#include "file_streaming.hpp"
#include "variational_parameters.hpp"
#include "vbayes_tracker.hpp"
#include "hyps.hpp"
#include "tools/eigen3.3/Dense"
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/device/file.hpp>
#include <boost/math/distributions/fisher_f.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/filesystem.hpp>

namespace boost_m = boost::math;
namespace boost_io = boost::iostreams;

template <typename T>
inline std::vector<int> validate_grid(const Eigen::MatrixXd &grid, T n_var);
inline Eigen::MatrixXd subset_matrix(const Eigen::MatrixXd &orig, const std::vector<int> &valid_points);

class VBayesX2 {
public:
	// Constants
	const double PI = 3.1415926535897;
	const double eps = std::numeric_limits<double>::min();
	const double alpha_tol = 1e-4;
	const double logw_tol = 1e-2;
	const double sigma_c = 10000;
	const int    n_chrs = 22; // Likely to break with X chromosome
	std::vector< std::string > covar_names;
	std::vector< std::string > env_names;

	// Column order of hyperparameters in grid
	const std::vector< std::string > hyps_names = {"sigma", "sigma_b", "sigma_g",
												  "lambda_b", "lambda_g"};

	// sizes
	int           n_effects;             // no. interaction variables + 1
	std::uint32_t n_samples;
	unsigned long n_covar;
	unsigned long n_env;
	std::uint32_t n_var;
	std::uint32_t n_var2;
	bool          random_params_init;
	bool          run_round1;
	double N; // (double) n_samples


	//
	parameters& p;
	std::vector< std::uint32_t > fwd_pass;
	std::vector< std::uint32_t > back_pass;
	std::vector< std::vector < std::uint32_t >> fwd_pass_chunks;
	std::vector< std::vector < std::uint32_t >> back_pass_chunks;
	std::vector< int > env_fwd_pass;
	std::vector< int > env_back_pass;
	std::map<unsigned long, Eigen::MatrixXd> D_correlations; //We can keep D^t D for the main effects

	// Data
	GenotypeMatrix&  X;
	EigenDataVector  Y;          // residual phenotype matrix
	EigenDataArrayX  Cty;         // vector of W^T x y where C the matrix of covariates
	EigenDataArrayXX E;          // matrix of variables used for GxE interactions
	EigenDataMatrix& C;          // matrix of covariates (superset of GxE variables)

	Eigen::ArrayXXd& dXtEEX;     // P x n_env^2; col (l * n_env + m) is the diagonal of X^T * diag(E_l * E_m) * X
	Eigen::MatrixXd r1_hyps_grid;
	Eigen::MatrixXd hyps_grid;

	// Global location of y_m = E[X beta] and y_x = E[X gamma]
	EigenDataMatrix YY, YX, YM, ETA, ETA_SQ;

	// genome wide scan computed upstream
	Eigen::ArrayXXd& snpstats;

	// Init points
	VariationalParametersLite vp_init;

	// boost fstreams
	boost_io::filtering_ostream outf, outf_map, outf_wmean, outf_nmean, outf_inits;
	boost_io::filtering_ostream outf_elbo, outf_alpha_diff, outf_map_pred, outf_weights;
	boost_io::filtering_ostream outf_rescan, outf_map_covar;

	// Monitoring
	std::chrono::system_clock::time_point time_check;
	std::chrono::duration<double> elapsed_innerLoop;

	explicit VBayesX2( Data& dat ) : X( dat.G ),
                            Y(Eigen::Map<EigenDataVector>(dat.Y.data(), dat.Y.rows())),
                            C( dat.W ),
                            dXtEEX( dat.dXtEEX ),
                            snpstats( dat.snpstats ),
                            p( dat.params ) {
		assert(std::includes(dat.hyps_names.begin(), dat.hyps_names.end(), hyps_names.begin(), hyps_names.end()));
		std::cout << "Initialising vbayes object" << std::endl;
		mkl_set_num_threads_local(p.n_thread);


		// Data size params
		n_effects      = dat.n_effects;
		n_var          = dat.n_var;
		n_env          = dat.n_env;
		n_var2         = n_effects * dat.n_var;
		n_samples      = dat.n_samples;
		n_covar        = dat.n_covar;
		covar_names    = dat.covar_names;
		env_names      = dat.env_names;
		N              = (double) n_samples;


		p.main_chunk_size = (unsigned int) std::min((long int) p.main_chunk_size, (long int) n_var);
		p.gxe_chunk_size = (unsigned int) std::min((long int) p.gxe_chunk_size, (long int) n_var);

		// Read environmental variables
		E = dat.E;

		// Allocate memory - fwd/back pass vectors
		std::cout << "Allocating indices for fwd/back passes" << std::endl;

		for(std::uint32_t kk = 0; kk < n_var * n_effects; kk++){
			fwd_pass.push_back(kk);
			back_pass.push_back(n_var2 - kk - 1);
		}

		for(int ll = 0; ll < n_env; ll++){
			env_fwd_pass.push_back(ll);
			env_back_pass.push_back(n_env - ll - 1);
		}

		unsigned long n_main_segs, n_gxe_segs, n_chunks;
 		n_main_segs = (n_var + p.main_chunk_size - 1) / p.main_chunk_size; // ceiling of n_var / chunk size
		n_gxe_segs = (n_var + p.gxe_chunk_size - 1) / p.gxe_chunk_size; // ceiling of n_var / chunk size
		n_chunks = n_main_segs;
		if(n_effects > 1){
			n_chunks += n_gxe_segs;
		}

		fwd_pass_chunks.resize(n_chunks);
		back_pass_chunks.resize(n_chunks);
		for(std::uint32_t kk = 0; kk < n_effects * n_var; kk++){
			std::uint32_t ch_index = (kk < n_var ? kk / p.main_chunk_size : n_main_segs + (kk % n_var) / p.gxe_chunk_size);
			fwd_pass_chunks[ch_index].push_back(kk);
			back_pass_chunks[n_chunks - 1 - ch_index].push_back(kk);
		}

//		for (auto chunk : fwd_pass_chunks){
//			for (auto ii : chunk){
//				std::cout << ii << " ";
//			}
//			std::cout << std::endl;
//		}
//
//		for (auto chunk : back_pass_chunks){
//			for (auto ii : chunk){
//				std::cout << ii << " ";
//			}
//			std::cout << std::endl;
//		}

		for (long ii = 0; ii < n_chunks; ii++){
			std::reverse(back_pass_chunks[ii].begin(), back_pass_chunks[ii].end());
		}

		// non random initialisation
		if(p.vb_init_file != "NULL"){
			std::cout << "Initialisation - set from file" << std::endl;

			// Main effects
			vp_init.alpha_beta     = dat.alpha_init.col(0);
			vp_init.mu1_beta       = dat.mu_init.col(0);
			vp_init.s1_beta_sq     = Eigen::ArrayXd::Zero(n_var);

			if(p.mode_mog_prior_beta){
				vp_init.mu2_beta   = Eigen::ArrayXd::Zero(n_var);
				vp_init.s2_beta_sq = Eigen::ArrayXd::Zero(n_var);
			}

			// Interaction effects
			if(n_effects > 1) {
				assert(dat.alpha_init.cols() > 1);

				vp_init.alpha_gam     = dat.alpha_init.col(1);
				vp_init.mu1_gam       = dat.mu_init.col(1);
				vp_init.s1_gam_sq     = Eigen::ArrayXd::Zero(n_var);

				if (p.mode_mog_prior_gam) {
					vp_init.mu2_gam   = Eigen::ArrayXd::Zero(n_var);
					vp_init.s2_gam_sq = Eigen::ArrayXd::Zero(n_var);
				}

				// Env Weights
				if(p.env_weights_file != "NULL"){
					vp_init.muw     = dat.E_weights.col(0);
				} else if (n_env > 1 && p.init_weights_with_snpwise_scan){
					calc_snpwise_regression(vp_init);
				} else {
					vp_init.muw.resize(n_env);
					vp_init.muw     = 1.0 / (double) n_env;
				}

				// cast used if DATA_AS_FLOAT
				vp_init.eta     = E.matrix() * vp_init.muw.matrix().cast<scalarData>();
				vp_init.eta_sq  = vp_init.eta.cwiseProduct(vp_init.eta);
			}

			// Covars
			if(p.use_vb_on_covars){
				vp_init.muc   = Eigen::ArrayXd::Zero(n_covar);
			}


			// ym, yx
			calcPredEffects(vp_init);

			random_params_init = false;
			run_round1         = false;
			if(p.user_requests_round1){
				run_round1     = true;
			}
		} else {
			random_params_init = true;
			run_round1         = true;
		}

		// Assign data - hyperparameters
		hyps_grid           = dat.hyps_grid;

		if(p.r1_hyps_grid_file == "NULL"){
			r1_hyps_grid    = hyps_grid;
		} else {
			r1_hyps_grid    = dat.r1_hyps_grid;
		}

		if(p.use_vb_on_covars){
			Cty = C.transpose() * Y;
		}
	}

	~VBayesX2(){
		// Close all ostreams
		boost_io::close(outf);
		boost_io::close(outf_map);
		boost_io::close(outf_wmean);
		boost_io::close(outf_nmean);
		boost_io::close(outf_elbo);
		boost_io::close(outf_alpha_diff);
		boost_io::close(outf_inits);
		boost_io::close(outf_rescan);
		boost_io::close(outf_map_covar);
	}

	void run(){
		std::cout << "Starting variational inference" << std::endl;
		time_check = std::chrono::system_clock::now();
		int n_thread = 1; // Parrallel starts swapped for multithreaded inference

		// Round 1; looking for best start point
		if(run_round1){

			std::vector< VbTracker > trackers(n_thread);
			long r1_n_grid = r1_hyps_grid.rows();
			run_inference(r1_hyps_grid, true, 1, trackers);

			if(p.verbose){
				write_trackers_to_file("round1_", trackers, r1_hyps_grid);
			}

			// Find best init
			double logw_best = -std::numeric_limits<double>::max();
			bool init_not_set = true;
			for (int ii = 0; ii < r1_n_grid; ii++){
				double logw      = trackers[ii].logw;
				if(std::isfinite(logw) && logw > logw_best){
					vp_init      = trackers[ii].vp;
					logw_best    = logw;
					init_not_set = false;
				}
			}

			if(init_not_set){
				throw std::runtime_error("No valid start points found (elbo estimates all non-finite?).");
			}

			// gen initial predicted effects
			calcPredEffects(vp_init);

			print_time_check();
		}

		// Write inits to file - exclude covar values
		std::string ofile_inits = fstream_init(outf_inits, "", "_inits");
		std::cout << "Writing start points for alpha and mu to " << ofile_inits << std::endl;
		write_snp_stats_to_file(outf_inits, n_effects, n_var, vp_init, X, p, false);
		boost_io::close(outf_inits);

		long n_grid = r1_hyps_grid.rows();
		std::vector< VbTracker > trackers(n_grid);
		run_inference(hyps_grid, false, 2, trackers);

		write_trackers_to_file("", trackers, hyps_grid);

		std::cout << "Variational inference finished" << std::endl;
	}

	void run_inference(const Eigen::Ref<const Eigen::MatrixXd>& hyps_grid,
                     const bool random_init,
                     const int round_index,
                     std::vector<VbTracker>& trackers){
		// Writes results from inference to trackers

		long n_grid = hyps_grid.rows();
		int n_thread = 1; // Parrallel starts swapped for multithreaded inference

		// Divide grid of hyperparameters into chunks for multithreading
		std::vector< std::vector< int > > chunks(n_thread);
		for (int ii = 0; ii < n_grid; ii++){
			int ch_index = (ii % n_thread);
			chunks[ch_index].push_back(ii);
		}

		// Allocate trackers
		assert(trackers.size() == n_grid);
		for (int nn = 0; nn < n_grid; nn++){
			trackers[nn].set_main_filepath(p.out_file);
			trackers[nn].p = p;
		}

		runOuterLoop(round_index, hyps_grid, n_grid, chunks[0], random_init, trackers);
	}

	void runOuterLoop(const int round_index,
                      const Eigen::Ref<const Eigen::MatrixXd>& outer_hyps_grid,
                      const unsigned long n_grid,
                      const std::vector<int>& grid_index_list,
                      const bool random_init,
                      std::vector<VbTracker>& all_tracker){

		std::vector<Hyps> all_hyps;
		unpack_hyps(outer_hyps_grid, all_hyps);

		// Run outer loop - don't update trackers
		auto innerLoop_start = std::chrono::system_clock::now();
		runInnerLoop(random_init, round_index, all_hyps, all_tracker);
		auto innerLoop_end = std::chrono::system_clock::now();
		elapsed_innerLoop = innerLoop_end - innerLoop_start;

		// 'rescan' GWAS of Z on y-ym
		if(n_effects > 1) {
			for (int nn = 0; nn < n_grid; nn++) {
				Eigen::VectorXd gam_neglogp(n_var);
				rescanGWAS(all_tracker[nn].vp, gam_neglogp);
				all_tracker[nn].push_rescan_gwas(X, n_var, gam_neglogp);
			}
		}
	}

	void unpack_hyps(const Eigen::Ref<const Eigen::MatrixXd>& outer_hyps_grid,
			std::vector<Hyps>& all_hyps){

		long n_grid = outer_hyps_grid.rows();
		for (int ii = 0; ii < n_grid; ii++) {
			Hyps i_hyps;
			if (n_effects == 2) {
				Eigen::ArrayXd muw_sq(n_env * n_env);
				for (int ll = 0; ll < n_env; ll++) {
					for (int mm = 0; mm < n_env; mm++) {
						muw_sq(mm * n_env + ll) = vp_init.muw(mm) * vp_init.muw(ll);
					}
				}
				double my_s_z = (dXtEEX.rowwise() * muw_sq.transpose()).sum() / (N - 1.0);

				i_hyps.init_from_grid(n_effects, ii, n_var, outer_hyps_grid, p, my_s_z);
			} else if (n_effects == 1){
				i_hyps.init_from_grid(n_effects, ii, n_var, outer_hyps_grid, p);
			}

			all_hyps.push_back(i_hyps);
		}
	}

	void runInnerLoop(const bool random_init,
                      const int round_index,
                      std::vector<Hyps>& all_hyps,
                      std::vector<VbTracker>& all_tracker){
		// minimise KL Divergence and assign elbo estimate
		// Assumes vp_init already exist
		// TODO: re intergrate random starts
		int print_interval = 25;
		if(random_init){
			throw std::logic_error("Random starts no longer implemented");
		}
		unsigned long n_grid = all_hyps.size();


		std::vector<VariationalParameters> all_vp;
		setup_variational_params(all_hyps, all_vp);

		// Run inner loop until convergence
		int count = 0;
		std::vector<int> converged(n_grid);
		bool all_converged = false;
		std::vector<Eigen::ArrayXd> alpha_prev(n_grid);
		std::vector<double> i_logw(n_grid);
		std::vector<std::vector< double >> logw_updates(n_grid), alpha_diff_updates(n_grid);

		for (int nn = 0; nn < n_grid; nn++){
			i_logw[nn] = -1*std::numeric_limits<double>::max();
			logw_updates[nn].push_back(i_logw[nn]);
			converged[nn] = 0;

			all_tracker[nn].interim_output_init(nn, round_index, n_effects, n_env, env_names, all_vp[nn]);
		}

		while(!all_converged && count < p.vb_iter_max){
			for (int nn = 0; nn < n_grid; nn++){
				alpha_prev[nn] = all_vp[nn].alpha_beta;
			}
			std::vector<double> logw_prev = i_logw;

			// WARNING: logw_prev copied by value (just want to check updates improve elbo)
			updateAllParams(count, round_index, all_vp, all_hyps, logw_prev, logw_updates);
			std::vector<double> alpha_diff(n_grid);
			for (int nn = 0; nn < n_grid; nn++){
				i_logw[nn]     = calc_logw(all_hyps[nn], all_vp[nn]);
				alpha_diff[nn] = (alpha_prev[nn] - all_vp[nn].alpha_beta).abs().maxCoeff();
				alpha_diff_updates[nn].push_back(alpha_diff[nn]);
			}

			// Interim output
			for (int nn = 0; nn < n_grid; nn++) {
				if (p.use_vb_on_covars && count % 10 == 0) {
					all_tracker[nn].push_interim_covar_values(count, n_covar, all_vp[nn], covar_names);
				}
				if (p.xtra_verbose && count % 20 == 0) {
					all_tracker[nn].push_interim_param_values(count, n_effects, n_var, all_vp[nn], X);
				}
				all_tracker[nn].push_interim_iter_update(count, all_hyps[nn], i_logw[nn], alpha_diff[nn], n_effects,
												 n_var, n_env, all_vp[nn]);
			}

			// Diagnose convergence
			for (int nn = 0; nn < n_grid; nn++) {
				double logw_diff = i_logw[nn] - logw_prev[nn];
				if (p.alpha_tol_set_by_user && p.elbo_tol_set_by_user) {
					if (alpha_diff[nn] < p.alpha_tol && logw_diff < p.elbo_tol) {
						converged[nn] = 1;
					}
				} else if (p.alpha_tol_set_by_user) {
					if (alpha_diff[nn] < p.alpha_tol) {
						converged[nn] = 1;
					}
				} else if (p.elbo_tol_set_by_user) {
					if (logw_diff < p.elbo_tol) {
						converged[nn] = 1;
					}
				} else {
					if (alpha_diff[nn] < alpha_tol && logw_diff < logw_tol) {
						converged[nn] = 1;
					}
				}
			}
			if (std::all_of(converged.begin(), converged.end(), [](int i){return i == 1;})){
				all_converged = true;
			}
			count++;

			// Report progress to std::cout
			if((count + 1) % print_interval == 0){
				int n_converged = 0;
				for (auto& n : converged){
					n_converged += n;
				}

				std::cout << "Completed " << count+1 << " iterations, " << n_converged << " runs converged";
				print_time_check();
			}
		}

		if(any_of(i_logw.begin(), i_logw.end(), [](double x) {return !std::isfinite(x);})){
			std::cout << "WARNING: non-finite elbo estimate produced" << std::endl;
		}

		// Log all things that we want to track
		for (int nn = 0; nn < n_grid; nn++) {
			all_tracker[nn].logw = i_logw[nn];
			all_tracker[nn].count = count;
			all_tracker[nn].vp = all_vp[nn].convert_to_lite();
			all_tracker[nn].hyps = all_hyps[nn];
			if (p.verbose) {
				logw_updates.push_back(i_logw);  // adding converged estimate
				all_tracker[nn].logw_updates = logw_updates[nn];
				all_tracker[nn].alpha_diffs = alpha_diff_updates[nn];
			}
			all_tracker[nn].push_interim_output(X, n_var, n_effects);
		}
	}

	void setup_variational_params(const std::vector<Hyps>& all_hyps,
			std::vector<VariationalParameters>& all_vp){
		unsigned long n_grid = all_hyps.size();

		// Init global locations YM YX
		YY.resize(n_samples, n_grid);
		YM.resize(n_samples, n_grid);
		for (int nn = 0; nn < n_grid; nn++){
			YM.col(nn) = vp_init.ym;
			YY.col(nn) = Y;
		}
		YX.resize(n_samples, n_grid);
		ETA.resize(n_samples, n_grid);
		ETA_SQ.resize(n_samples, n_grid);
		if (n_effects > 1){
			for (int nn = 0; nn < n_grid; nn++){
				YX.col(nn) = vp_init.yx;
				ETA.col(nn) = vp_init.eta;
				ETA_SQ.col(nn) = vp_init.eta_sq;
			}
		}

		// Init variational params
		for (int nn = 0; nn < n_grid; nn++){
			VariationalParameters vp(YM.col(nn), YX.col(nn), ETA.col(nn), ETA_SQ.col(nn));
			vp.init_from_lite(vp_init);
			updateSSq(all_hyps[nn], vp);


			all_vp.push_back(vp);
		}
	}

	/********** VB update functions ************/
	void updateAllParams(const int& count,
			             const int& round_index,
			             std::vector<VariationalParameters>& all_vp,
						 std::vector<Hyps>& all_hyps,
						 std::vector<double> logw_prev,
						 std::vector<std::vector< double >>& logw_updates){
		std::vector< std::uint32_t > iter;
		std::vector< std::vector< std::uint32_t >> iter_chunks;
		unsigned long n_grid = all_hyps.size();
		std::vector<double> i_logw(n_grid);

		// Alternate between back and fwd passes
		bool is_fwd_pass = (count % 2 == 0);
		if(is_fwd_pass){
			iter = fwd_pass;
			iter_chunks = fwd_pass_chunks;
		} else {
			iter = back_pass;
			iter_chunks = back_pass_chunks;
		}

		// Update covar main effects
		for (int nn = 0; nn < n_grid; nn++) {
			if (p.use_vb_on_covars) {
				updateCovarEffects(all_vp[nn], all_hyps[nn]);
				check_monotonic_elbo(all_hyps[nn], all_vp[nn], count, logw_prev[nn], "updateCovarEffects");
			}
		}

		// Update main & interaction effects
		updateAlphaMu(iter_chunks, all_hyps, all_vp, is_fwd_pass);

		for (int nn = 0; nn < n_grid; nn++) {
			check_monotonic_elbo(all_hyps[nn], all_vp[nn], count, logw_prev[nn], "updateAlphaMu");

			// Update env-weights
			if (n_env > 1) {
				for (int uu = 0; uu < p.env_update_repeats; uu++) {
					updateEnvWeights(env_fwd_pass, all_hyps[nn], all_vp[nn]);
					updateEnvWeights(env_back_pass, all_hyps[nn], all_vp[nn]);
				}
				check_monotonic_elbo(all_hyps[nn], all_vp[nn], count, logw_prev[nn], "updateEnvWeights");
			}

			// Log updates
			i_logw[nn] = calc_logw(all_hyps[nn], all_vp[nn]);
			double alpha_diff = 0;

			compute_pve(all_hyps[nn]);

			// Maximise hyps
			if (round_index > 1 && p.mode_empirical_bayes) {
				if (count >= p.burnin_maxhyps) maximiseHyps(all_hyps[nn], all_vp[nn]);

				i_logw[nn] = calc_logw(all_hyps[nn], all_vp[nn]);

				compute_pve(all_hyps[nn]);
			}
			logw_updates[nn].push_back(i_logw[nn]);
		}
	}

	void updateCovarEffects(VariationalParameters& vp,
                            const Hyps& hyps) __attribute__ ((hot)){
		//
		for (int cc = 0; cc < n_covar; cc++){
			double rr_k = vp.muc(cc);

			// Update s_sq
			vp.sc_sq(cc) = hyps.sigma * sigma_c / (sigma_c * (N - 1.0) + 1.0);

			// Update mu
			auto A = Cty(cc) - (vp.ym + vp.yx.cwiseProduct(vp.eta)).dot(C.col(cc));
			vp.muc(cc) = vp.sc_sq(cc) * ( (double) A + rr_k * (N - 1.0)) / hyps.sigma;

			// Update predicted effects
			double rr_k_diff     = vp.muc(cc) - rr_k;
			vp.ym += rr_k_diff * C.col(cc);
		}
	}

	void updateAlphaMu(const std::vector< std::vector< std::uint32_t >>& iter_chunks,
                       const std::vector<Hyps>& all_hyps,
                       std::vector<VariationalParameters>& all_vp,
                       const bool& is_fwd_pass){
		// Divide updates into chunks
		// Partition chunks amongst available threads
		unsigned long n_grid = all_hyps.size();
		EigenDataMatrix D;
		Eigen::MatrixXd AA;     // snp_batch x n_grid
		Eigen::MatrixXd rr_diff;                   // snp_batch x n_grid

		for (std::uint32_t ch = 0; ch < iter_chunks.size(); ch++){
			std::vector< std::uint32_t > chunk = iter_chunks[ch];
			int ee                 = chunk[0] / n_var;
			unsigned long ch_len   = chunk.size();

			// D is n_samples x snp_batch
			if(D.cols() != ch_len){
				D.resize(n_samples, ch_len);
			}
			if(rr_diff.rows() != ch_len){
				rr_diff.resize(ch_len, n_grid);
			}
			if(AA.rows() != ch_len){
				AA.resize(ch_len, n_grid);
			}
			X.col_block3(chunk, D);

			// Most work done here
			// AA is snp_batch x n_grid
			AA = computeGeneResidualCorrelation(D, ee);

			// Update parameters based on AA
			for (int nn = 0; nn < n_grid; nn++) {
				Eigen::Ref<Eigen::VectorXd> A = AA.col(nn);

				unsigned long memoize_id = ((is_fwd_pass) ? ch : ch + iter_chunks.size());
				adjustParams(nn, memoize_id, chunk, D, A, all_hyps, all_vp, rr_diff);
			}

			// Update residuals
#ifdef DATA_AS_FLOAT
			if(ee == 0){
				YM.noalias() += D * rr_diff.cast<float>();
			} else {
				YX.noalias() += D * rr_diff.cast<float>();
			}
#else
			if(ee == 0){
				YM.noalias() += D * rr_diff;
			} else {
				YX.noalias() += D * rr_diff;
			}
#endif
		}

		for (int nn = 0; nn < n_grid; nn++){
			// update summary quantity
			all_vp[nn].calcVarqBeta(all_hyps[nn], p, n_effects);
		}
	}

	void adjustParams(const int& nn, const unsigned long& memoize_id,
			const std::vector<std::uint32_t>& chunk,
			const EigenDataMatrix& D,
			const Eigen::Ref<const Eigen::VectorXd>& A,
			const std::vector<Hyps>& all_hyps,
			std::vector<VariationalParameters>& all_vp,
			Eigen::Ref<Eigen::MatrixXd> rr_diff){

		int ee                 = chunk[0] / n_var;
		unsigned long ch_len   = chunk.size();
		if (ee == 0) {

			// Update main effects
			if (D_correlations.count(memoize_id) == 0) {
				if(p.n_thread == 1) {
					Eigen::MatrixXd D_corr(ch_len, ch_len);
					// cast only used if DATA_AS_FLOAT
					D_corr.triangularView<Eigen::StrictlyUpper>() = (D.transpose() * D).template cast<double>();
					D_correlations[memoize_id] = D_corr;
				} else {
					D_correlations[memoize_id] = (D.transpose() * D).template cast<double>();
				}
			}

			_internal_updateAlphaMu_beta(chunk, A, D_correlations[memoize_id], all_hyps[nn], all_vp[nn], rr_diff.col(nn));
		} else {

			// Update interaction effects
			Eigen::MatrixXd D_corr(ch_len, ch_len);
			if(p.gxe_chunk_size > 1) {
				if(p.n_thread == 1){
					// cast only used if DATA_AS_FLOAT
					D_corr.triangularView<Eigen::StrictlyUpper>() = (D.transpose() * all_vp[nn].eta_sq.asDiagonal() * D).template cast<double>();
				} else {
					D_corr = (D.transpose() * all_vp[nn].eta_sq.asDiagonal() * D).template cast<double>();
				}
			}

			_internal_updateAlphaMu_gam(chunk, A, D_corr, all_hyps[nn], all_vp[nn], rr_diff.col(nn));
		}
	}

	template <typename EigenMat>
	Eigen::MatrixXd computeGeneResidualCorrelation(const EigenMat& D,
			const int& ee){
		// Most work done here
		// variant correlations with residuals
		EigenMat res;
		if(n_effects == 1){
			// Main effects update in main effects only model
			res.noalias() = (YY - YM).transpose() * D;
			res.transposeInPlace();
		} else if (ee == 0){
			// Main effects update in interaction model
			res.noalias() = (YY - YM - YX.cwiseProduct(ETA)).transpose() * D;
			res.transposeInPlace();
		} else {
			// Interaction effects
			res.noalias() = D.transpose() * ((YY - YM).cwiseProduct(ETA) - YX.cwiseProduct(ETA_SQ));
		}
		// cast only used if DATA_AS_FLOAT
		return(res.template cast<double>()); // n_grid x snp_batch
	}

	void _internal_updateAlphaMu_beta(const std::vector< std::uint32_t >& iter_chunk,
									  const Eigen::Ref<const Eigen::VectorXd>& A,
									  const Eigen::Ref<const Eigen::MatrixXd>& D_corr,
									  const Hyps& hyps,
									  VariationalParameters& vp,
									  Eigen::Ref<Eigen::MatrixXd> rr_k_diff){

		unsigned long ch_len = iter_chunk.size();
		int ee = 0;

		Eigen::ArrayXd alpha_cnst;
		if(p.mode_mog_prior_beta){
			alpha_cnst  = (hyps.lambda / (1.0 - hyps.lambda) + eps).log();
			alpha_cnst -= (hyps.slab_var.log() - hyps.spike_var.log()) / 2.0;
		} else {
			alpha_cnst = (hyps.lambda / (1.0 - hyps.lambda) + eps).log() - hyps.slab_var.log() / 2.0;
		}

		// adjust updates within chunk
		Eigen::VectorXd rr_k(ch_len);
		assert(rr_k_diff.rows() == ch_len);
		for (int ii = 0; ii < ch_len; ii++){
			std::uint32_t jj = iter_chunk[ii];

			// Log prev value
			rr_k(ii)                            = vp.alpha_beta(jj) * vp.mu1_beta(jj);
			if(p.mode_mog_prior_beta) rr_k(ii) += (1.0 - vp.alpha_beta(jj)) * vp.mu2_beta(jj);

			// Update s_sq
			vp.s1_beta_sq(jj)                        = hyps.slab_var(ee);
			vp.s1_beta_sq(jj)                       /= (hyps.slab_relative_var(ee) * (N-1) + 1);
			if(p.mode_mog_prior_beta) vp.s2_beta_sq(jj)  = hyps.spike_var(ee);
			if(p.mode_mog_prior_beta) vp.s2_beta_sq(jj) /= (hyps.spike_relative_var(ee) * (N-1) + 1);

			// Update mu
			double offset = rr_k(ii) * (N-1.0);
			for (int mm = 0; mm < ii; mm++){
				offset -= rr_k_diff(mm, 0) * D_corr(mm, ii);
			}
			double AA = A(ii) + offset;
			vp.mu1_beta(jj)                            = vp.s1_beta_sq(jj) * AA / hyps.sigma;
			if (p.mode_mog_prior_beta) vp.mu2_beta(jj) = vp.s2_beta_sq(jj) * AA / hyps.sigma;


			// Update alpha
			double ff_k;
			ff_k                        = vp.mu1_beta(jj) * vp.mu1_beta(jj) / vp.s1_beta_sq(jj);
			ff_k                       += std::log(vp.s1_beta_sq(jj));
			if (p.mode_mog_prior_beta) ff_k -= vp.mu2_beta(jj) * vp.mu2_beta(jj) / vp.s2_beta_sq(jj);
			if (p.mode_mog_prior_beta) ff_k -= std::log(vp.s2_beta_sq(jj));
			vp.alpha_beta(jj)           = sigmoid(ff_k / 2.0 + alpha_cnst(ee));

			rr_k_diff(ii, 0)                       = vp.alpha_beta(jj) * vp.mu1_beta(jj) - rr_k(ii);
			if(p.mode_mog_prior_beta) rr_k_diff(ii, 0) += (1.0 - vp.alpha_beta(jj)) * vp.mu2_beta(jj);
		}
	}

	void _internal_updateAlphaMu_gam(const std::vector< std::uint32_t >& iter_chunk,
									 const Eigen::Ref<const Eigen::VectorXd>& A,
									 const Eigen::Ref<const Eigen::MatrixXd>& D_corr,
									 const Hyps& hyps,
									 VariationalParameters& vp,
									 Eigen::Ref<Eigen::MatrixXd> rr_k_diff){

		int ch_len = iter_chunk.size();
		int ee     = 1;

		Eigen::ArrayXd alpha_cnst;
		if(p.mode_mog_prior_gam){
			alpha_cnst  = (hyps.lambda / (1.0 - hyps.lambda) + eps).log();
			alpha_cnst -= (hyps.slab_var.log() - hyps.spike_var.log()) / 2.0;
		} else {
			alpha_cnst = (hyps.lambda / (1.0 - hyps.lambda) + eps).log() - hyps.slab_var.log() / 2.0;
		}

		// Vector of previous values
		Eigen::VectorXd rr_k(ch_len);
		for (int ii = 0; ii < ch_len; ii++){
			std::uint32_t jj = iter_chunk[ii] % n_var;
			rr_k(ii)                       = vp.alpha_gam(jj) * vp.mu1_gam(jj);
			if(p.mode_mog_prior_gam) rr_k(ii) += (1.0 - vp.alpha_gam(jj)) * vp.mu2_gam(jj);
		}

		// adjust updates within chunk
		// Need to be able to go backwards during a back_pass
		assert(rr_k_diff.rows() == ch_len);
		for (int ii = 0; ii < ch_len; ii++){
			std::uint32_t jj = (iter_chunk[ii] % n_var); // variant index

			// Update s_sq
			vp.s1_gam_sq(jj)                        = hyps.slab_var(ee);
			vp.s1_gam_sq(jj)                       /= (hyps.slab_relative_var(ee) * vp.EdZtZ(jj) + 1);
			if(p.mode_mog_prior_gam) vp.s2_gam_sq(jj)  = hyps.spike_var(ee);
			if(p.mode_mog_prior_gam) vp.s2_gam_sq(jj) /= (hyps.spike_relative_var(ee) * vp.EdZtZ(jj) + 1);

			// Update mu
			double offset = rr_k(ii) * vp.EdZtZ(jj);
			for (int mm = 0; mm < ii; mm++){
				offset -= rr_k_diff(mm, 0) * D_corr(mm, ii);
			}
			double AA = A(ii) + offset;
			vp.mu1_gam(jj)                       = vp.s1_gam_sq(jj) * AA / hyps.sigma;
			if (p.mode_mog_prior_gam) vp.mu2_gam(jj) = vp.s2_gam_sq(jj) * AA / hyps.sigma;


			// Update alpha
			double ff_k;
			ff_k                        = vp.mu1_gam(jj) * vp.mu1_gam(jj) / vp.s1_gam_sq(jj);
			ff_k                       += std::log(vp.s1_gam_sq(jj));
			if (p.mode_mog_prior_gam) ff_k -= vp.mu2_gam(jj) * vp.mu2_gam(jj) / vp.s2_gam_sq(jj);
			if (p.mode_mog_prior_gam) ff_k -= std::log(vp.s2_gam_sq(jj));
			vp.alpha_gam(jj)           = sigmoid(ff_k / 2.0 + alpha_cnst(ee));

			rr_k_diff(ii, 0)                       = vp.alpha_gam(jj) * vp.mu1_gam(jj) - rr_k(ii);
			if(p.mode_mog_prior_gam) rr_k_diff(ii, 0) += (1.0 - vp.alpha_gam(jj)) * vp.mu2_gam(jj);
		}
	}

	void updateSSq(const Hyps& hyps,
                   VariationalParameters& vp){
		// Used only when initialising VB.
		// We compute elbo on starting point hence need variance estimates
		// Also resizes all variance arrays.

		// Update beta ssq
		int ee = 0;
		vp.s1_beta_sq.resize(n_var);
		vp.s1_beta_sq  = hyps.slab_var(ee);
		vp.s1_beta_sq /= hyps.slab_relative_var(ee) * (N - 1.0) + 1.0;

		if(p.mode_mog_prior_beta) {
			vp.s2_beta_sq.resize(n_var);
			vp.s2_beta_sq = hyps.spike_var(ee);
			vp.s2_beta_sq /= (hyps.spike_relative_var(ee) * (N - 1.0) + 1.0);
		}

		if(n_effects > 1) {
			// Would need vp.s_sq to compute properly, which in turn needs vp.sw_sq...
			vp.sw_sq.resize(n_env);
			vp.sw_sq = eps;
			vp.calcEdZtZ(dXtEEX, n_env);

			// Update gamma ssq
			ee = 1;
			vp.s1_gam_sq.resize(n_var);
			vp.s1_gam_sq = hyps.slab_var(ee);
			vp.s1_gam_sq /= (hyps.slab_relative_var(ee) * (N - 1.0) + 1.0);

			if (p.mode_mog_prior_gam) {
				vp.s2_gam_sq.resize(n_var);
				vp.s2_gam_sq = hyps.spike_var(ee);
				vp.s2_gam_sq /= (hyps.spike_relative_var(ee) * (N - 1.0) + 1.0);
			}
		}

		vp.varB.resize(n_var);
		vp.varG.resize(n_var);
		vp.calcVarqBeta(hyps, p, n_effects);

		// for covars
		if(p.use_vb_on_covars){
			vp.sc_sq.resize(n_covar);
			for (int cc = 0; cc < n_covar; cc++){
				vp.sc_sq(cc) = hyps.sigma * sigma_c / (sigma_c * (N - 1.0) + 1.0);
			}
		}
	}

	void maximiseHyps(Hyps& hyps,
                    const VariationalParameters& vp){

		// max sigma
		hyps.sigma  = calcExpLinear(hyps, vp);
		if (p.use_vb_on_covars){
			hyps.sigma += (vp.sc_sq + vp.muc.square()).sum() / sigma_c;
			hyps.sigma /= (N + (double) n_covar);
		} else {
			hyps.sigma /= N;
		}

		// beta - max lambda
		int ee = 0;
		hyps.lambda[ee] = vp.alpha_beta.sum();

		// beta - max spike & slab variances
		hyps.slab_var[ee]  = (vp.alpha_beta * (vp.s1_beta_sq + vp.mu1_beta.square())).sum();
		hyps.slab_var[ee] /= hyps.lambda[ee];
		hyps.slab_relative_var[ee] = hyps.slab_var[ee] / hyps.sigma;
		if(p.mode_mog_prior_beta){
			hyps.spike_var[ee]  = ((1.0 - vp.alpha_beta) * (vp.s2_beta_sq + vp.mu2_beta.square())).sum();
			hyps.spike_var[ee] /= ( (double)n_var - hyps.lambda[ee]);
			hyps.spike_relative_var[ee] = hyps.spike_var[ee] / hyps.sigma;
		}

		// beta - finish max lambda
		hyps.lambda[ee] /= n_var;

		// gamma
		if(n_effects > 1){
			ee = 1;
			hyps.lambda[ee] = vp.alpha_gam.sum();

			// max spike & slab variances
			hyps.slab_var[ee]  = (vp.alpha_gam * (vp.s1_gam_sq + vp.mu1_gam.square())).sum();
			hyps.slab_var[ee] /= hyps.lambda[ee];
			hyps.slab_relative_var[ee] = hyps.slab_var[ee] / hyps.sigma;
			if(p.mode_mog_prior_gam){
				hyps.spike_var[ee]  = ((1.0 - vp.alpha_gam) * (vp.s2_gam_sq + vp.mu2_gam.square())).sum();
				hyps.spike_var[ee] /= ( (double)n_var - hyps.lambda[ee]);
				hyps.spike_relative_var[ee] = hyps.spike_var[ee] / hyps.sigma;
			}

			// finish max lambda
			hyps.lambda[ee] /= n_var;
		}

//		// max spike & slab variances
//		hyps.slab_var.resize(n_effects);
//		hyps.spike_var.resize(n_effects);
//		for (int ee = 0; ee < n_effects; ee++){
//
//			// Initial unconstrained max
//			hyps.slab_var[ee]  = (vp.alpha.col(ee) * (vp.s_sq.col(ee) + vp.mu.col(ee).square())).sum();
//			hyps.slab_var[ee] /= hyps.lambda[ee];
//			if(p.mode_mog_prior){
//				hyps.spike_var[ee]  = ((1.0 - vp.alpha.col(ee)) * (vp.sp_sq.col(ee) + vp.mup.col(ee).square())).sum();
//				hyps.spike_var[ee] /= ( (double)n_var - hyps.lambda[ee]);
//			}
//
//			// Remaxise while maintaining same diff in MoG variances if getting too close
//			if(p.mode_mog_prior && hyps.slab_var[ee] < p.min_spike_diff_factor * hyps.spike_var[ee]){
//				hyps.slab_var[ee]  = (vp.alpha.col(ee) * (vp.s_sq.col(ee) + vp.mu.col(ee).square())).sum();
//				hyps.slab_var[ee] += p.min_spike_diff_factor * ((1.0 - vp.alpha.col(ee)) * (vp.sp_sq.col(ee) + vp.mup.col(ee).square())).sum();
//				hyps.slab_var[ee] /= (double) n_var;
//				hyps.spike_var[ee] = hyps.slab_var[ee] / p.min_spike_diff_factor;
//			}
//		}
//
//		hyps.slab_relative_var = hyps.slab_var / hyps.sigma;
//		if(p.mode_mog_prior){
//			hyps.spike_relative_var = hyps.spike_var / hyps.sigma;
//		}

		// hyps.lam_b         = hyps.lambda(0);
		// hyps.lam_g         = hyps.lambda(1);
		// hyps.sigma_b       = hyps.slab_relative_var(0);
		// hyps.sigma_g       = hyps.slab_relative_var(1);
		// hyps.sigma_g_spike = hyps.spike_relative_var(0);
		// hyps.sigma_g_spike = hyps.spike_relative_var(1);
	}

	void updateEnvWeights(const std::vector< int >& iter,
                          Hyps& hyps,
                          VariationalParameters& vp){

		for (int ll : iter){
			// Log previous mean weight
			double r_ll = vp.muw(ll);

			// Update s_sq
			double denom = hyps.sigma;
			denom       += (vp.yx.array() * E.col(ll)).square().sum();
			denom       += (vp.varG * dXtEEX.col(ll*n_env + ll)).sum();
			vp.sw_sq(ll) = hyps.sigma / denom;

			// Remove dependance on current weight
			vp.eta -= (r_ll * E.col(ll)).matrix();

			// Update mu
			Eigen::ArrayXd env_vars = Eigen::ArrayXd::Zero(n_var);
			for (int mm = 0; mm < n_env; mm++){
				if(mm != ll){
					env_vars += vp.muw(mm) * dXtEEX.col(ll*n_env + mm);
				}
			}

			double eff = ((Y - vp.ym).array() * E.col(ll) * vp.yx.array()).sum();
			eff       -= (vp.yx.array() * E.col(ll) * vp.eta.array() * vp.yx.array()).sum();
			eff       -= (vp.varG * env_vars).sum();
			vp.muw(ll) = vp.sw_sq(ll) * eff / hyps.sigma;

			// Update eta
			vp.eta += (vp.muw(ll) * E.col(ll)).matrix();
		}

		// rescale weights such that vector eta has variance 1
//		if(p.rescale_eta){
//			double sigma = (vp.eta.array() - (vp.eta.array().sum() / N)).matrix().squaredNorm() / (N-1);
//			vp.muw /= std::sqrt(sigma);
//			vp.eta = E.matrix() * vp.muw.matrix();
//			double sigma2 = (vp.eta.array() - (vp.eta.array().sum() / N)).matrix().squaredNorm() / (N-1);
//			std::cout << "Eta rescaled; variance " << sigma << " -> variance " << sigma2 <<std::endl;
//		}

		// Recompute eta_sq
		vp.eta_sq  = vp.eta.array().square().matrix();
#ifdef DATA_AS_FLOAT
		vp.eta_sq += E.square().matrix() * vp.sw_sq.matrix().cast<float>();
#else
		vp.eta_sq += E.square().matrix() * vp.sw_sq.matrix();
#endif

		// Recompute expected value of diagonal of ZtZ
		vp.calcEdZtZ(dXtEEX, n_env);

		// Compute s_x; sum of column variances of Z
		Eigen::ArrayXd muw_sq(n_env * n_env);
		for (int ll = 0; ll < n_env; ll++){
			for (int mm = 0; mm < n_env; mm++){
				muw_sq(mm*n_env + ll) = vp.muw(mm) * vp.muw(ll);
			}
		}
		// WARNING: Hard coded limit!
		// WARNING: Updates S_x in hyps
		hyps.s_x(0) = (double) n_var;
		hyps.s_x(1) = (dXtEEX.rowwise() * muw_sq.transpose()).sum() / (N - 1.0);
	}

	double calc_logw(const Hyps& hyps,
                     const VariationalParameters& vp){

		// Expectation of linear regression log-likelihood
		double int_linear = -1.0 * calcExpLinear(hyps, vp) / 2.0 / hyps.sigma;
		int_linear -= N * std::log(2.0 * PI * hyps.sigma) / 2.0;

		// gamma
		int ee;
		double col_sum, int_gamma = 0;
		ee = 0;
		col_sum = vp.alpha_beta.sum();
		int_gamma += col_sum * std::log(hyps.lambda(ee) + eps);
		int_gamma -= col_sum * std::log(1.0 - hyps.lambda(ee) + eps);
		int_gamma += (double) n_var *  std::log(1.0 - hyps.lambda(ee) + eps);

		if(n_effects > 1) {
			ee = 1;
			col_sum = vp.alpha_gam.sum();
			int_gamma += col_sum * std::log(hyps.lambda(ee) + eps);
			int_gamma -= col_sum * std::log(1.0 - hyps.lambda(ee) + eps);
			int_gamma += (double) n_var * std::log(1.0 - hyps.lambda(ee) + eps);
		}

		// kl-beta
		double int_klbeta = calcIntKLBeta(hyps, vp);
		if(n_effects > 1) {
			int_klbeta += calcIntKLGamma(hyps, vp);
		}

		// covariates
		double kl_covar = 0.0;
		if(p.use_vb_on_covars){
			kl_covar += (double) n_covar * (1.0 - hyps.sigma * sigma_c) / 2.0;
			kl_covar += vp.sc_sq.log().sum() / 2.0;
			kl_covar -= vp.sc_sq.sum() / 2.0 / hyps.sigma / sigma_c;
			kl_covar -= vp.muc.square().sum() / 2.0 / hyps.sigma / sigma_c;
		}

		// weights
		double kl_weights = 0.0;
		if(n_env > 1) {
			kl_weights += (double) n_env / 2.0;
			kl_weights += vp.sw_sq.log().sum() / 2.0;
			kl_weights -= vp.sw_sq.sum() / 2.0;
			kl_weights -= vp.muw.square().sum() / 2.0;
		}

		double res = int_linear + int_gamma + int_klbeta + kl_covar + kl_weights;

		return res;
	}

	void calc_snpwise_regression(VariationalParametersLite& vp){
		/*
		Genome-wide gxe scan now computed upstream
		Eigen::ArrayXXd snpstats contains results

		cols:
		neglogp-main, neglogp-gxe, coeff-gxe-main, coeff-gxe-env..
		*/

		// Keep values from point with highest p-val
		vp.muw = Eigen::ArrayXd::Zero(n_env);
		double vv = 0.0;
		for (long int jj = 0; jj < n_var; jj++){
			if (snpstats(jj, 1) > vv){
				vv = snpstats(jj, 1);
				vp.muw = snpstats.block(jj, 2, 1, n_env).transpose();

				std::cout << "neglogp at variant " << jj << ": " << vv;
				std::cout << std::endl << vp.muw.transpose() << std::endl;
			}
		}
	}

	/********** Helper functions ************/
	void print_time_check(){
		auto now = std::chrono::system_clock::now();
		std::chrono::duration<double> elapsed_seconds = now-time_check;
		std::cout << " (" << elapsed_seconds.count();
		std::cout << " seconds since last timecheck, estimated RAM usage = ";
		std::cout << getValueRAM() << "KB)" << std::endl;
		time_check = now;
	}

	void initRandomAlphaMu(VariationalParameters& vp){
		// vp.alpha a uniform simplex, vp.mu standard gaussian
		// Also sets predicted effects
		std::default_random_engine gen_gauss, gen_unif;
		std::normal_distribution<double> gaussian(0.0,1.0);
		std::uniform_real_distribution<double> uniform(0.0,1.0);

		// Beta
		vp.mu1_beta.resize(n_var);
		vp.alpha_beta.resize(n_var);
		if(p.mode_mog_prior_beta){
			vp.mu2_beta = Eigen::ArrayXd::Zero(n_var);
		}

		for (std::uint32_t kk = 0; kk < n_var; 	kk++){
			vp.alpha_beta(kk) = uniform(gen_unif);
			vp.mu1_beta(kk)    = gaussian(gen_gauss);
		}
		vp.alpha_beta /= vp.alpha_beta.sum();

		// Gamma
		if(n_effects > 1){
			vp.mu1_gam.resize(n_var);
			vp.alpha_gam.resize(n_var);
			if (p.mode_mog_prior_gam) {
				vp.mu2_gam = Eigen::ArrayXd::Zero(n_var);
			}

			for (std::uint32_t kk = 0; kk < n_var; kk++) {
				vp.alpha_gam(kk) = uniform(gen_unif);
				vp.mu1_gam(kk) = gaussian(gen_gauss);
			}
			vp.alpha_gam /= vp.alpha_gam.sum();
		}

		if(p.use_vb_on_covars){
			vp.muc = Eigen::ArrayXd::Zero(n_covar);
		}

		// Gen predicted effects.
		calcPredEffects(vp);

		// Env weights - cast if DATA_AS_FLOAT
		vp.muw     = 1.0 / (double) n_env;
#ifdef DATA_AS_FLOAT
		vp.eta     = E.matrix() * vp.muw.matrix().cast<float>();
#else
		vp.eta     = E.matrix() * vp.muw.matrix();
#endif
		vp.eta_sq  = vp.eta.array().square();
		vp.calcEdZtZ(dXtEEX, n_env);
	}

	void calcPredEffects(VariationalParameters& vp){
		Eigen::VectorXd rr_beta, rr_gam;
		if(p.mode_mog_prior_beta){
			rr_beta = vp.alpha_beta * (vp.mu1_beta - vp.mu2_beta) + vp.mu2_beta;
		} else {
			rr_beta = vp.alpha_beta * vp.mu1_beta;
		}

		vp.ym = X * rr_beta;
		if(p.use_vb_on_covars){
#ifdef DATA_AS_FLOAT
			vp.ym += C * vp.muc.matrix().cast<float>();
#else
			vp.ym += C * vp.muc.matrix();
#endif
		}

		if(n_effects > 1) {
			if (p.mode_mog_prior_gam) {
				rr_gam = vp.alpha_gam * (vp.mu1_gam - vp.mu2_gam) + vp.mu2_gam;
			} else {
				rr_gam = vp.alpha_gam * vp.mu1_gam;
			}
			vp.yx = X * rr_gam;
		}
	}

	void calcPredEffects(VariationalParametersLite& vp) {
		Eigen::VectorXd rr_beta, rr_gam;
		if(p.mode_mog_prior_beta){
			rr_beta = vp.alpha_beta * (vp.mu1_beta - vp.mu2_beta) + vp.mu2_beta;
		} else {
			rr_beta = vp.alpha_beta * vp.mu1_beta;
		}

		vp.ym = X * rr_beta;
		if(p.use_vb_on_covars){
#ifdef DATA_AS_FLOAT
			vp.ym += C * vp.muc.matrix().cast<float>();
#else
			vp.ym += C * vp.muc.matrix();
#endif
		}

		if(n_effects > 1) {
			if (p.mode_mog_prior_gam) {
				rr_gam = vp.alpha_gam * (vp.mu1_gam - vp.mu2_gam) + vp.mu2_gam;
			} else {
				rr_gam = vp.alpha_gam * vp.mu1_gam;
			}
			vp.yx = X * rr_gam;
		}
	}

	void check_monotonic_elbo(const Hyps& hyps,
                   VariationalParameters& vp,
                   const int count,
                   double& logw_prev,
                   const std::string& prev_function){
		double i_logw     = calc_logw(hyps, vp);
		if(i_logw < logw_prev){
			std::cout << count << ": " << prev_function;
 			std::cout << " " << logw_prev << " -> " << i_logw;
 			std::cout << " (difference of " << i_logw - logw_prev << ")"<< std::endl;
		}
		logw_prev = i_logw;
	}

	void normaliseLogWeights(std::vector< double >& my_weights){
		// Safer to normalise log-weights than niavely convert to weights
		// Skip non-finite values!
		int nn = my_weights.size();
		double max_elem = *std::max_element(my_weights.begin(), my_weights.end());
		for (int ii = 0; ii < nn; ii++){
			my_weights[ii] = std::exp(my_weights[ii] - max_elem);
		}

		double my_sum = 0.0;
		for (int ii = 0; ii < nn; ii++){
			if(std::isfinite(my_weights[ii])){
				my_sum += my_weights[ii];
			}
		}

		for (int ii = 0; ii < nn; ii++){
			my_weights[ii] /= my_sum;
		}

		int nonfinite_count = 0;
		for (int ii = 0; ii < nn; ii++){
			if(!std::isfinite(my_weights[ii])){
				nonfinite_count++;
			}
		}

		if(nonfinite_count > 0){
			std::cout << "WARNING: " << nonfinite_count << " grid points returned non-finite ELBO.";
			std::cout << "Skipping these when producing posterior estimates.";
		}
	}

	double calcExpLinear(const Hyps& hyps,
                         const VariationalParameters& vp){
		// Expectation of ||Y - C tau - X beta - Z gamma||^2
		double int_linear = 0;

		// Expectation of linear regression log-likelihood
		int_linear  = (Y - vp.ym).squaredNorm();
		if(n_effects > 1) {
			int_linear -= 2.0 * (Y - vp.ym).cwiseProduct(vp.eta).dot(vp.yx);
			if (n_env > 1) {
				int_linear += vp.yx.cwiseProduct(vp.eta_sq).dot(vp.yx);
			} else {
				int_linear += vp.yx.cwiseProduct(vp.eta).squaredNorm();
			}
		}

		// variances
		if(p.use_vb_on_covars){
			int_linear += (N - 1.0) * vp.sc_sq.sum(); // covar main
		}
		int_linear += (N - 1.0) * vp.varB.sum();  // beta
		if(n_effects > 1) {
			int_linear += (vp.EdZtZ * vp.varG).sum(); // gamma
		}

		return int_linear;
	}

	double calcIntKLBeta(const Hyps& hyps,
						 const VariationalParameters& vp){
		// KL Divergence of log[ p(beta | u, theta) / q(u, beta) ]
		double col_sum, res;
		int ee = 0;

		// beta
		if(p.mode_mog_prior_beta){
			res  = n_var / 2.0;

			res -= (vp.alpha_beta * (vp.mu1_beta.square() + vp.s1_beta_sq)).sum() / 2.0 / hyps.slab_var(ee);
			res += (vp.alpha_beta * vp.s1_beta_sq.log()).sum() / 2.0;

			res -= ((1.0 - vp.alpha_beta) * (vp.mu2_beta.square() + vp.s2_beta_sq)).sum() / 2.0 / hyps.spike_var(ee);
			res += ((1.0 - vp.alpha_beta) * vp.s2_beta_sq.log()).sum() / 2.0;

			col_sum = vp.alpha_beta.sum();
			res -= std::log(hyps.slab_var(ee))  * col_sum / 2.0;
			res -= std::log(hyps.spike_var(ee)) * (n_var - col_sum) / 2.0;
		} else {
			res  = (vp.alpha_beta * vp.s1_beta_sq.log()).sum() / 2.0;
			res -= (vp.alpha_beta * (vp.mu1_beta.square() + vp.s1_beta_sq)).sum() / 2.0 / hyps.slab_var(ee);

			col_sum = vp.alpha_beta.sum();
			res += col_sum * (1 - std::log(hyps.slab_var(ee))) / 2.0;
		}

		for (std::uint32_t kk = 0; kk < n_var; kk++){
			res -= vp.alpha_beta(kk) * std::log(vp.alpha_beta(kk) + eps);
			res -= (1 - vp.alpha_beta(kk)) * std::log(1 - vp.alpha_beta(kk) + eps);
		}
		return res;
	}

	double calcIntKLGamma(const Hyps& hyps,
						  const VariationalParameters& vp){
		// KL Divergence of log[ p(beta | u, theta) / q(u, beta) ]
		double col_sum, res;
		int ee = 1;

		// beta
		if(p.mode_mog_prior_gam){
			res  = n_var / 2.0;

			res -= (vp.alpha_gam * (vp.mu1_gam.square() + vp.s1_gam_sq)).sum() / 2.0 / hyps.slab_var(ee);
			res += (vp.alpha_gam * vp.s1_gam_sq.log()).sum() / 2.0;

			res -= ((1.0 - vp.alpha_gam) * (vp.mu2_gam.square() + vp.s2_gam_sq)).sum() / 2.0 / hyps.spike_var(ee);
			res += ((1.0 - vp.alpha_gam) * vp.s2_gam_sq.log()).sum() / 2.0;

			col_sum = vp.alpha_gam.sum();
			res -= std::log(hyps.slab_var(ee))  * col_sum / 2.0;
			res -= std::log(hyps.spike_var(ee)) * (n_var - col_sum) / 2.0;
		} else {
			res  = (vp.alpha_gam * vp.s1_gam_sq.log()).sum() / 2.0;
			res -= (vp.alpha_gam * (vp.mu1_gam.square() + vp.s1_gam_sq)).sum() / 2.0 / hyps.slab_var(ee);

			col_sum = vp.alpha_gam.sum();
			res += col_sum * (1 - std::log(hyps.slab_var(ee))) / 2.0;
		}

		for (std::uint32_t kk = 0; kk < n_var; kk++){
			res -= vp.alpha_gam(kk) * std::log(vp.alpha_gam(kk) + eps);
			res -= (1 - vp.alpha_gam(kk)) * std::log(1 - vp.alpha_gam(kk) + eps);
		}
		return res;
	}

	void compute_pve(Hyps& hyps){
		// Compute heritability
		hyps.pve.resize(n_effects);
		hyps.pve_large.resize(n_effects);

		hyps.pve = hyps.lambda * hyps.slab_relative_var * hyps.s_x;
		if(p.mode_mog_prior_beta){
			int ee = 0;
			hyps.pve_large[ee] = hyps.pve[ee];
			hyps.pve[ee] += (1 - hyps.lambda[ee]) * hyps.spike_relative_var[ee] * hyps.s_x[ee];

			if (p.mode_mog_prior_gam && n_effects > 1){
				int ee = 1;
				hyps.pve_large[ee] = hyps.pve[ee];
				hyps.pve[ee] += (1 - hyps.lambda[ee]) * hyps.spike_relative_var[ee] * hyps.s_x[ee];
			}

			hyps.pve_large[ee] /= (hyps.pve.sum() + 1.0);
		}
		hyps.pve /= (hyps.pve.sum() + 1.0);
	}

	void rescanGWAS(const VariationalParametersLite& vp,
					Eigen::Ref<Eigen::VectorXd> neglogp){
		// casts used is DATA_AS_FLOAT
		Eigen::VectorXd pheno = (Y.cast<double>() - vp.ym.cast<double>());
		Eigen::VectorXd Z_kk(n_samples);

		for(std::uint32_t jj = 0; jj < n_var; jj++ ){
			Z_kk = (X.col(jj).cast<double>().cwiseProduct(vp.eta.cast<double>()));
			double ztz_inv = 1.0 / Z_kk.dot(Z_kk);
			double gam = Z_kk.dot(pheno) * ztz_inv;
			double rss_null = (pheno - Z_kk * gam).squaredNorm();

			// T-test of variant j
			boost_m::students_t t_dist(n_samples - 1);
			double main_se_j    = std::sqrt(rss_null / (N - 1.0) * ztz_inv);
			double main_tstat_j = gam / main_se_j;
			double main_pval_j  = 2 * boost_m::cdf(boost_m::complement(t_dist, fabs(main_tstat_j)));

			neglogp(jj) = -1 * std::log10(main_pval_j);
		}
	}

	void compute_residuals_per_chr(const VariationalParametersLite& vp,
			std::vector<Eigen::VectorXd>& chr_residuals){

		std::set<int> chrs(X.chromosome.begin(), X.chromosome.end());
		assert(chr_residuals.size() == n_chrs);

		// casts used if DATA_AS_FLOAT
		Eigen::VectorXd map_residuals;
		if (n_effects > 1) {
			map_residuals = (Y - vp.ym - vp.ym.cwiseProduct(vp.eta)).cast<double>();
		} else {
			map_residuals = (Y - vp.ym).cast<double>();
		}

		// Compute predicted effects from each chromosome
		Eigen::VectorXd Eq_beta, Eq_gam;
		std::vector<Eigen::VectorXd> pred_main(n_chrs), pred_int(n_chrs);

		Eq_beta = vp.alpha_beta * vp.mu1_beta;
		if(p.mode_mog_prior_beta) Eq_beta.array() += (1 - vp.alpha_beta) * vp.mu2_beta;
		for (auto cc : chrs){
			pred_main[cc] = X.mult_vector_by_chr(cc, Eq_beta);
		}

		if (n_effects > 1) {
			Eq_gam  = vp.alpha_gam  * vp.mu1_gam;
			if(p.mode_mog_prior_gam) Eq_gam.array() += (1 - vp.alpha_gam) * vp.mu2_gam;
			for (auto cc : chrs){
				pred_int[cc]  = X.mult_vector_by_chr(cc, Eq_gam);
			}
		}

		// Compute mean-centered residuals for each chromosome
		for (auto cc : chrs){
			if (n_effects > 1){
				chr_residuals[cc] = map_residuals + pred_main[cc] + pred_int[cc].cwiseProduct(vp.eta.cast<double>());
			} else {
				chr_residuals[cc] = map_residuals + pred_main[cc];
			}
			chr_residuals[cc].array() -= chr_residuals[cc].mean();
		}
	}

	void LOCO_pvals(const VariationalParametersLite& vp,
					const std::vector<Eigen::VectorXd>& chr_residuals,
					Eigen::Ref<Eigen::VectorXd> neglogp_beta,
					Eigen::Ref<Eigen::VectorXd> neglogp_gam,
					Eigen::Ref<Eigen::VectorXd> neglogp_joint){
		assert(neglogp_beta.rows()  == n_var);
		assert(neglogp_gam.rows()   == n_var);
		assert(neglogp_joint.rows() == n_var);
		assert(n_effects == 1 || n_effects == 2);

		std::set<int> chrs(X.chromosome.begin(), X.chromosome.end());
		assert(chr_residuals.size() == n_chrs);

		// Compute p-vals per variant (p=3 as residuals mean centered)
		Eigen::MatrixXd H_kk(n_samples, n_effects);
		boost_m::students_t t_dist(n_samples - n_effects - 1);
		boost_m::fisher_f f_dist(n_effects, n_samples - n_effects - 1);
		for(std::uint32_t jj = 0; jj < n_var; jj++ ){
			int chr = X.chromosome[jj];
			H_kk.col(0) = X.col(jj).cast<double>();

			if(n_effects == 1){
				double ztz_inv = 1.0 / H_kk.squaredNorm();
				double tau = (H_kk.transpose() * chr_residuals[chr])(0,0) * ztz_inv;
				double rss_null = (chr_residuals[chr] - H_kk * tau).squaredNorm();
				// T-test of variant j
				double main_se_j    = std::sqrt(rss_null / (N - 1.0) * ztz_inv);
				double main_tstat_j = tau / main_se_j;
				double main_pval_j  = 2 * boost_m::cdf(boost_m::complement(t_dist, fabs(main_tstat_j)));

				neglogp_beta(jj) = -1 * std::log10(main_pval_j);
			} else if (n_effects > 1){
				H_kk.col(1) = H_kk.col(0).cwiseProduct(vp.eta.cast<double>());

				// Model Fitting - p=3 as residuals mean centered
				Eigen::Matrix2d HtH     = H_kk.transpose() * H_kk;
				Eigen::Matrix2d HtH_inv = HtH.inverse(); // should be efficient for 2x2 matrix
				Eigen::Vector2d tau     = HtH_inv * H_kk.transpose() * chr_residuals[chr];

				double rss_null = chr_residuals[chr].squaredNorm();
				double rss_alt  = (chr_residuals[chr] - H_kk * tau).squaredNorm();

				// T-test on beta
				double beta_tstat = tau[0] / sqrt(rss_alt * HtH_inv(0, 0) / (N - 3.0));
				double beta_pval  = 2 * boost_m::cdf(boost_m::complement(t_dist, fabs(beta_tstat)));
				neglogp_beta[jj]  = -1 * std::log10(beta_pval);

				// T-test on gamma
				double gam_tstat  = tau[1] / sqrt(rss_alt * HtH_inv(1, 1) / (N - 3.0));
				double gam_pval   = 2 * boost_m::cdf(boost_m::complement(t_dist, fabs(gam_tstat)));
				neglogp_gam[jj]   = -1 * std::log10(gam_pval);

				// F-test over main+int effects of snp_j
				double joint_fstat, joint_pval;
				joint_fstat       = (rss_null - rss_alt) / 2.0;
				joint_fstat      /= rss_alt / (N - 3.0);
				joint_pval        = 1.0 - boost_m::cdf(f_dist, joint_fstat);
				neglogp_joint[jj] = -1 * std::log10(joint_pval);
			}
		}
	}

	/********** Output functions ************/
	void write_trackers_to_file(const std::string& file_prefix,
                                const std::vector< VbTracker >& trackers,
                                const Eigen::Ref<const Eigen::MatrixXd>& hyps_grid){
		// Stitch trackers back together if using multithreading
		int n_thread = 1; // Parrallel starts swapped for multithreaded inference
		unsigned long my_n_grid = hyps_grid.rows();
		output_init(file_prefix);
		output_results(trackers, my_n_grid);
	}

	void output_init(const std::string& file_prefix){
		// Initialise files ready to write;

		std::string ofile       = fstream_init(outf, file_prefix, "");
		std::string ofile_map   = fstream_init(outf_map, file_prefix, "_map_snp_stats");
		std::string ofile_wmean = fstream_init(outf_wmean, file_prefix, "_weighted_mean_snp_stats");
		std::string ofile_nmean = fstream_init(outf_nmean, file_prefix, "_niave_mean_snp_stats");
		std::string ofile_map_yhat = fstream_init(outf_map_pred, file_prefix, "_map_yhat");
		std::string ofile_w = fstream_init(outf_weights, file_prefix, "_env_weights");
		std::string ofile_rescan = fstream_init(outf_rescan, file_prefix, "_map_rescan");
		std::string ofile_map_covar = fstream_init(outf_map_covar, file_prefix, "_map_covar");
		std::cout << "Writing converged hyperparameter values to " << ofile << std::endl;
		std::cout << "Writing MAP snp stats to " << ofile_map << std::endl;
		std::cout << "Writing MAP covar coefficients to " << ofile_map_covar << std::endl;
		std::cout << "Writing (weighted) average snp stats to " << ofile_wmean << std::endl;
		std::cout << "Writing (niave) average snp stats to " << ofile_nmean << std::endl;
		std::cout << "Writing yhat from map to " << ofile_map_yhat << std::endl;
		std::cout << "Writing env weights to " << ofile_w << std::endl;
		std::cout << "Writing 'rescan' p-values of MAP to " << ofile_rescan << std::endl;

		if(p.verbose){
			std::string ofile_elbo = fstream_init(outf_elbo, file_prefix, "_elbo");
			std::cout << "Writing ELBO from each VB iteration to " << ofile_elbo << std::endl;

			std::string ofile_alpha_diff = fstream_init(outf_alpha_diff, file_prefix, "_alpha_diff");
			std::cout << "Writing max change in alpha from each VB iteration to " << ofile_alpha_diff << std::endl;
		}
	}

	void output_results(const std::vector<VbTracker>& trackers, const int my_n_grid){
		// Write;
		// main output; weights logw converged_hyps counts time (currently no prior)
		// snps;
		// - map (outf_map) / elbo weighted mean (outf_wmean) / niave mean + sds (outf_nmean)
		// (verbose);
		// - elbo trajectories (inside the interim files?)
		// - hyp trajectories (inside the interim files)

		// Compute normalised weights using finite elbo
		std::vector< double > weights(my_n_grid);
		if(my_n_grid > 1){
			for (int ii = 0; ii < my_n_grid; ii++){
				if(p.mode_empirical_bayes){
					weights[ii] = trackers[ii].logw;
				}
			}
			normaliseLogWeights(weights);
		} else {
			weights[0] = 1;
		}

		/*** Hyps - header ***/
		outf << "weight logw count sigma";

		for (int ee = 0; ee < n_effects; ee++){
			outf << " pve" << ee;
			if((ee == 0 && p.mode_mog_prior_beta) || (ee == 1 && p.mode_mog_prior_gam)){
				outf << " pve_large" << ee;
 			}
			outf << " sigma" << ee;
			if((ee == 0 && p.mode_mog_prior_beta) || (ee == 1 && p.mode_mog_prior_gam)){
				outf << " sigma_spike" << ee;
				outf << " sigma_spike_dilution" << ee;
			}
			outf << " lambda" << ee;
		}
		outf << std::endl;

		/*** Hyps - converged ***/
		for (int ii = 0; ii < my_n_grid; ii++){
			outf << std::setprecision(4);
			outf << weights[ii];
			outf << " " << trackers[ii].logw;
			outf << " " << trackers[ii].count;
			outf << " " << trackers[ii].hyps.sigma;

			outf << std::setprecision(8) << std::fixed;
			for (int ee = 0; ee < n_effects; ee++){

				// PVE
				outf << " " << trackers[ii].hyps.pve(ee);
				if((ee == 0 && p.mode_mog_prior_beta) || (ee == 1 && p.mode_mog_prior_gam)){
					outf << " " << trackers[ii].hyps.pve_large(ee);
				}

				// MoG variances
				outf << std::scientific << std::setprecision(5);
				outf << " " << trackers[ii].hyps.slab_relative_var(ee);
				outf << std::setprecision(8) << std::fixed;

				if((ee == 0 && p.mode_mog_prior_beta) || (ee == 1 && p.mode_mog_prior_gam)){
					outf << std::scientific << std::setprecision(5);
					outf << " " << trackers[ii].hyps.spike_relative_var(ee);

					outf << std::setprecision(3) << std::fixed;
					outf << " " << trackers[ii].hyps.slab_relative_var(ee) / trackers[ii].hyps.spike_relative_var(ee);
					outf << std::setprecision(8) << std::fixed;
				}

				// Lambda
				outf << " " << trackers[ii].hyps.lambda(ee);
			}
			outf << std::endl;
		}

		/*********** Stats from MAP to file ************/
		std::vector<Eigen::VectorXd> map_residuals_by_chr(n_chrs);
		long int ii_map = std::distance(weights.begin(), std::max_element(weights.begin(), weights.end()));
		VariationalParametersLite vp_map = trackers[ii_map].vp;

		// Predicted effects to file
		calcPredEffects(vp_map);
		compute_residuals_per_chr(vp_map, map_residuals_by_chr);
		if(n_effects == 1) {
			outf_map_pred << "Xbeta" << std::endl;
			for (std::uint32_t ii = 0; ii < n_samples; ii++) {
				outf_map_pred << vp_map.ym(ii) << std::endl;
			}
		} else {
			outf_map_pred << "Xbeta eta Xgamma" << std::endl;
			for (std::uint32_t ii = 0; ii < n_samples; ii++) {
				outf_map_pred << vp_map.ym(ii) << " " << vp_map.eta(ii) << " " << vp_map.yx(ii) << std::endl;
			}
		}

		// weights to file
		for (int ll = 0; ll < n_env; ll++){
			outf_weights << env_names[ll];
			if(ll + 1 < n_env) outf_weights << " ";
		}
		outf_weights << std::endl;
		for (int ll = 0; ll < n_env; ll++){
			outf_weights << vp_map.muw(ll);
			if(ll + 1 < n_env) outf_weights << " ";
		}
		outf_weights << std::endl;

		// Compute LOCO p-values
		Eigen::VectorXd neglogp_beta(n_var), neglogp_gam(n_var), neglogp_joint(n_var);
		LOCO_pvals(vp_map, map_residuals_by_chr, neglogp_beta, neglogp_gam, neglogp_joint);

		// MAP snp-stats to file
		write_snp_stats_to_file(outf_map, n_effects, n_var, vp_map, X, p, true, neglogp_beta, neglogp_gam, neglogp_joint);
		if(p.use_vb_on_covars) {
			write_covars_to_file(outf_map_covar, vp_map);
		}

		// Rescan of map
		if(n_env > 1) {
			Eigen::VectorXd gam_neglogp(n_var);
			rescanGWAS(trackers[ii_map].vp, gam_neglogp);
			outf_rescan << "chr rsid pos a0 a1 maf info neglogp" << std::endl;
			for (std::uint32_t kk = 0; kk < n_var; kk++) {
				outf_rescan << X.chromosome[kk] << " " << X.rsid[kk] << " " << X.position[kk];
				outf_rescan << " " << X.al_0[kk] << " " << X.al_1[kk] << " ";
				outf_rescan << X.maf[kk] << " " << X.info[kk] << " " << gam_neglogp(kk);
				outf_rescan << std::endl;
			}
		}


		if(p.verbose){
			outf_elbo << std::setprecision(4) << std::fixed;
			for (int ii = 0; ii < my_n_grid; ii++){
				for (int cc = 0; cc < trackers[ii].logw_updates.size(); cc++){
					outf_elbo << trackers[ii].logw_updates[cc] << " ";
				}
				outf_elbo << std::endl;
			}

			outf_alpha_diff << std::setprecision(4) << std::fixed;
			for (int ii = 0; ii < my_n_grid; ii++){
				for (int cc = 0; cc < trackers[ii].alpha_diffs.size(); cc++){
					outf_alpha_diff << trackers[ii].alpha_diffs[cc] << " ";
				}
				outf_alpha_diff << std::endl;
			}
		}
	}

	void write_covars_to_file(boost_io::filtering_ostream& ofile,
								 VariationalParametersLite vp) {
		// Assumes ofile has been initialised.

		// Header
		ofile << "covar beta" << std::endl;

		ofile << std::setprecision(9) << std::fixed;
		for (int cc = 0; cc < n_covar; cc++) {
			ofile << covar_names[cc] << " " << vp.muc(cc) << std::endl;
		}
	}

	std::string fstream_init(boost_io::filtering_ostream& my_outf,
                             const std::string& file_prefix,
                             const std::string& file_suffix){

		std::string filepath   = p.out_file;
		std::string dir        = filepath.substr(0, filepath.rfind('/')+1);
		std::string stem_w_dir = filepath.substr(0, filepath.find('.'));
		std::string stem       = stem_w_dir.substr(stem_w_dir.rfind('/')+1, stem_w_dir.size());
		std::string ext        = filepath.substr(filepath.find('.'), filepath.size());

		std::string ofile      = dir + file_prefix + stem + file_suffix + ext;

		my_outf.reset();
		std::string gz_str = ".gz";
		if (p.out_file.find(gz_str) != std::string::npos) {
			my_outf.push(boost_io::gzip_compressor());
		}
		my_outf.push(boost_io::file_sink(ofile));
		return ofile;
	}

	void check_inputs(){
		// If grid contains hyperparameter values that aren't sensible then we exclude
		assert(Y.rows() == n_samples);
		assert(X.rows() == n_samples);
		long n_grid = hyps_grid.rows();

		std::vector<int> valid_points, r1_valid_points;
		valid_points        = validate_grid(hyps_grid, n_var);
		hyps_grid           = subset_matrix(hyps_grid, valid_points);

		if(valid_points.empty()){
			throw std::runtime_error("No valid grid points in hyps_grid.");
		} else if(n_grid > valid_points.size()){
			std::cout << "WARNING: " << n_grid - valid_points.size();
			std::cout << " invalid grid points removed from hyps_grid." << std::endl;
		}

		// r1_hyps_grid assigned during constructor (ie before this function call)
		long r1_n_grid   = r1_hyps_grid.rows();
		r1_valid_points = validate_grid(r1_hyps_grid, n_var);
		r1_hyps_grid    = subset_matrix(r1_hyps_grid, r1_valid_points);

		if(r1_valid_points.empty()){
			throw std::runtime_error("No valid grid points in r1_hyps_grid.");
		} else if(r1_n_grid > r1_valid_points.size()){
			std::cout << "WARNING: " << r1_n_grid - r1_valid_points.size();
			std::cout << " invalid grid points removed from r1_hyps_grid." << std::endl;
		}
	}

	int parseLineRAM(char* line){
		// This assumes that a digit will be found and the line ends in " Kb".
		std::size_t i = strlen(line);
		const char* p = line;
		while (*p <'0' || *p > '9') p++;
		line[i-3] = '\0';
		char* s_end;
		int res = atoi(p);
		return res;
	}

	int getValueRAM(){ //Note: this value is in KB!
#ifndef OSX
		FILE* file = fopen("/proc/self/status", "r");
		int result = -1;
		char line[128];

		while (fgets(line, 128, file) != NULL){
			if (strncmp(line, "VmRSS:", 6) == 0){
				result = parseLineRAM(line);
				break;
			}
		}
		fclose(file);
		return result;
#else
		return -1;
#endif
	}
};

template <typename T>
inline std::vector<int> validate_grid(const Eigen::MatrixXd &grid, const T n_var){
	const int sigma_ind   = 0;
	const int sigma_b_ind = 1;
	const int sigma_g_ind = 2;
	const int lam_b_ind   = 3;
	const int lam_g_ind   = 4;

	std::vector<int> valid_points;
	for (int ii = 0; ii < grid.rows(); ii++){
		double lam_b = grid(ii, lam_b_ind);
		double lam_g = grid(ii, lam_g_ind);

		bool chck_sigma   = (grid(ii, sigma_ind)   >  0.0 && std::isfinite(grid(ii, sigma_ind)));
		bool chck_sigma_b = (grid(ii, sigma_b_ind) >  0.0 && std::isfinite(grid(ii, sigma_b_ind)));
		bool chck_sigma_g = (grid(ii, sigma_g_ind) >= 0.0 && std::isfinite(grid(ii, sigma_g_ind)));
		bool chck_lam_b   = (lam_b >= 1.0 / (double) n_var) && (lam_b < 1.0) && std::isfinite(lam_b);
		bool chck_lam_g   = (lam_g >= 0) && (lam_g < 1.0) && std::isfinite(lam_g);
		if(chck_lam_b && chck_lam_g && chck_sigma && chck_sigma_g && chck_sigma_b){
			valid_points.push_back(ii);
		}
	}
	return valid_points;
}

inline Eigen::MatrixXd subset_matrix(const Eigen::MatrixXd &orig, const std::vector<int> &valid_points){
	long n_cols = orig.cols(), n_rows = valid_points.size();
	Eigen::MatrixXd subset(n_rows, n_cols);

	for(int kk = 0; kk < n_rows; kk++){
		for(int jj = 0; jj < n_cols; jj++){
			subset(kk, jj) = orig(valid_points[kk], jj);
		}
	}
	return subset;
}
#endif
