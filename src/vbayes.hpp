// class for implementation of variational bayes algorithm
#ifndef VBAYES_HPP
#define VBAYES_HPP

#include <iostream>
#include <cmath>
#include <limits>
#include <random>
#include "data.hpp"
#include "tools/eigen3.3/Dense"
#include "tools/eigen3.3/Sparse"
#include "tools/eigen3.3/Eigenvalues"
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/device/file.hpp>
#include <boost/iostreams/filter/gzip.hpp>


inline double sigmoid(double x);
inline double int_klbeta(Eigen::VectorXd alpha,
						 Eigen::VectorXd mu, 
						 std::vector< double > s_sq,
						 double var,
						 int n_var,
						 double eps);


// TODO: Minimise deep copy by passing arguments by reference


class vbayes {
	// Niave implementation of vbayes algorithm as defined here:
	// https://projecteuclid.org/euclid.ba/1339616726

	public:
	int n_grid; // size of grid

	double n_samples;  // TODO: check type is appropriate
	std::size_t n_var;
	double PI = 3.1415926535897;
	int iter_max = 1000;
	double diff_tol = 1e-4;
	double eps = std::numeric_limits<double>::min();

	Eigen::MatrixXd X;     // dosage matrix - reference variable!
	Eigen::MatrixXd Y;     // residual phenotype matrix - reference variable!
	Eigen::VectorXd dXtX;  // diagonal of X^T * X
	Eigen::MatrixXd Xty;
	Eigen::VectorXd rr;    // column vector of elements rr[kk] = alpha[kk]mu[kk]
	Eigen::MatrixXd hyps_grid;
	Eigen::MatrixXd probs_grid; // prob of each point in grid under hyps

	// Stores values of alpha / mu over importance sampling grid
	std::vector< Eigen::VectorXd > alpha_i;
	std::vector< Eigen::VectorXd > mu_i;

	std::vector< int > fwd_pass;
	std::vector< int > back_pass;

	// posteriors
	std::vector< double > weights;
	std::vector< double > alpha_av, mu_av, beta_av;


	int sigma_ind;
	int sig_b_ind;
	int pi_ind;

	vbayes( data& dat ) : X( dat.G ),
 						  Y( dat.Y ) {
		sigma_ind = 0;
		sig_b_ind = 1;
		pi_ind = 2;
		std::vector< std::string > hyps_names;
		hyps_names.push_back("sigma_e");
		hyps_names.push_back("sigma_b");
		hyps_names.push_back("pi");
		assert(dat.hyps_names == hyps_names);

		// Data size params
		n_var = dat.n_var;
		n_samples = (double) dat.n_samples;
		n_grid = dat.hyps_grid.rows();

		probs_grid = dat.imprt_grid;
		hyps_grid = dat.hyps_grid;
		dXtX = (X.transpose() * X).diagonal();
		Xty = X.transpose() * Y;
	}

	// For use in unit testing.
	vbayes(Eigen::MatrixXd myX, Eigen::MatrixXd myY) : X( myX ), Y( myY ){
		dXtX = (X.transpose() * X).diagonal();
		Xty = X.transpose() * Y;
		n_samples = X.rows();
		n_var = X.cols();
	}

	~vbayes(){
	}

	void check_inputs(){
		assert(Y.rows() == n_samples);
		assert(X.rows() == n_samples);

		for (int ii = 0; ii < n_grid; ii++){
			assert(hyps_grid(ii, sigma_ind) > 0.0);
			assert(hyps_grid(ii, sig_b_ind) > 0.0);
			assert(hyps_grid(ii, pi_ind) > 0.0);
			assert(hyps_grid(ii, pi_ind) < 1.0);
		}
	}

	void random_alpha_mu(Eigen::VectorXd& alpha,
					   Eigen::VectorXd& mu){
		std::default_random_engine gen_gauss, gen_unif;
		std::normal_distribution<double> gaussian(0.0,1.0);
		std::uniform_real_distribution<double> uniform(0.0,1.0);
		double my_sum = 0;

		// Check alpha / mu are correct size
		alpha.resize(n_var);
		mu.resize(n_var);

		// Random initialisation of alpha, mu
		for (int kk = 0; kk < n_var; kk++){
			alpha(kk) = uniform(gen_unif);
			mu(kk) = gaussian(gen_gauss);
			my_sum += alpha(kk);
		}

		// Convert alpha to simplex. Not sure why this is a good idea
		for (int kk = 0; kk < n_var; kk++){
			alpha(kk) /= my_sum;
		}
	}

	void inner_loop_update(Eigen::RowVectorXd hyps, 
					Eigen::VectorXd& alpha,
					Eigen::VectorXd& mu,
					Eigen::VectorXd& Xr,
					std::vector< int > iter){
		// Inner loop as described in Carbonetto & Stephens Fig 1
		// alpha & mu passed by reference.
		// 
		// Return number of loops until convergence
		double sigma, sigmab, pi, ff, rr_k, s_sq;
		int kk;

		sigma = hyps(sigma_ind);
		sigmab = hyps(sig_b_ind);
		pi = hyps(pi_ind);

		for (int jj = 0; jj < n_var; jj++){

			kk = iter[jj];

			rr_k = alpha(kk) * mu(kk);
			s_sq = sigmab * sigma / (sigmab * dXtX(kk) + 1.0);

			// Update mu (eq 9)
			mu(kk) = s_sq / sigma;
			mu(kk) *= (Xty(kk, 0) - Xr.dot(X.col(kk)) + dXtX(kk) * rr_k);

			// Update alpha (eq 10)
			ff = std::log(pi / (1.0 - pi)) + std::log(s_sq / sigmab / sigma);
			ff += mu(kk) * mu(kk) / s_sq / 2.0;
			alpha(kk) = sigmoid(ff);

			Xr = Xr + (alpha(kk)*mu(kk) - rr_k) * X.col(kk);
		}
	}

	double calc_logw(double sigma, double sigmab, double pi,
				   std::vector< double > s_sq,
				   Eigen::VectorXd& alpha,
				   Eigen::VectorXd& mu){
		// Uses dXtX, X and Y from class namespace
		double res = 0.0;
		assert(mu.rows() == n_var);
		assert(alpha.rows() == n_var);
		assert(s_sq.size() == n_var);

		// gen Var[B_k]
		Eigen::VectorXd varB(n_var);
		for (int kk = 0; kk < n_var; kk++){
			double mu_sq = mu(kk) * mu(kk);
			varB(kk) = alpha(kk)*(s_sq[kk] + mu_sq) - alpha(kk) * alpha(kk) * mu_sq;
		}

		// gen rr
		rr = (alpha.array() * mu.array()).matrix();

		// Expectation of linear regression log-likelihood
		res -= n_samples * std::log(2.0 * PI * sigma) / 2.0;
		res -= (Y - X * rr).squaredNorm() / 2.0 / sigma;
		res -= 0.5 * (dXtX.dot(varB)) / sigma;

		// Expectation of prior inclusion probabilities
		for (int kk = 0; kk < n_var; kk++){
			res += alpha(kk) * std::log(pi + eps);
			res += (1.0 - alpha(kk)) * std::log(1.0 - pi + eps);
		}

		// Negative KL divergaence between priors and approx distribution
		double var = sigma * sigmab;
		res += int_klbeta(alpha, mu, s_sq, var, n_var, eps);

		return res;
	}

	double outer_loop(Eigen::RowVectorXd hyps,
					Eigen::VectorXd alpha,
					Eigen::VectorXd mu){
		int cnt;
		std::vector< double > s_sq(n_var, 0);
		std::vector< int > iter;
		Eigen::VectorXd alpha0, mu0, Xr;
		double diff, sigma, sigmab, pi, logw;

		sigma = hyps(sigma_ind);
		sigmab = hyps(sig_b_ind);
		pi = hyps(pi_ind);

		// Useful quantities
		rr = alpha.cwiseProduct(mu);
		Xr = X * rr;

		// solve for s_sq (eq 8)
		for (int kk = 0; kk < n_var; kk++){
			s_sq[kk] = sigmab * sigma / (sigmab * dXtX(kk) + 1.0);
		}

		// Start inner loop
		std::cout << "Starting inner loop" << std::endl;
		for(int ll = 0; ll < iter_max; ll++){
			alpha0 = alpha;
			mu0 = mu;

			// Alternate between forward & backward passes
			if(ll % 2 == 0){
				iter = fwd_pass;
			} else {
				iter = back_pass;
			}

			// Variational inference; update alpha, beta, Xr
			std::cout << "Starting update" << std::endl;
			inner_loop_update(hyps, alpha, mu, Xr, iter);

			// Break maximum change in mixing coefficients is less than some
			// tolerance.
			diff = (alpha0 - alpha).cwiseAbs().maxCoeff();
			if(diff > diff_tol){
				break;
			}
		}

		// log-lik lower bound logw (eq 14)
		logw = calc_logw(sigma, sigmab, pi, s_sq, alpha, mu);

		return logw;
	}

	void run(){
		Eigen::RowVectorXd hyps;
		Eigen::VectorXd alpha, mu, alpha1, mu1, Xr, rr;
		std::vector< double > s_sq(n_var, 0);
		double sigma, sigmab, pi, logw, logw1;
		int cnt;
		bool check = false;

		logw1 = -1.0 * std::numeric_limits<double>::max();

		// Allocate memory
		weights.resize(n_grid, 0.0);
		rr.resize(n_var);
		alpha_i.resize(n_grid);
		mu_i.resize(n_grid);

		// Initialise forward & backward pass vectors
		for(int kk = 0; kk < n_var; kk++){
			fwd_pass.push_back(kk);
			back_pass.push_back(n_var - kk - 1);
		}

		// First run with random alpha / mu
		for (int ii = 0; ii < n_grid; ii++){
			std::cout << "\rRound 1: grid point " << ii+1 << "/" << n_grid;
			hyps = hyps_grid.row(ii);

			// Initialise alpha and mu randomly
			random_alpha_mu(alpha, mu);

			std::cout << "Starting outer loop" << std::endl;
			logw = outer_loop(hyps, alpha, mu);
			if (logw > logw1){
				check = true;
				logw1 = logw;
				alpha1 = alpha;
				mu1 = mu;
			}
		}

		if(!check){
			throw std::runtime_error("ERROR: No valid common starting points found.");
		}

		// Second run with best guess alpha / mu
		for (int ii = 0; ii < n_grid; ii++){
			std::cout << "\rRound 2: grid point " << ii+1 << "/" << n_grid;
			hyps = hyps_grid.row(ii);

			// Choose best init for alpha and mu
			alpha = alpha1;
			mu = mu1;

			logw = outer_loop(hyps, alpha, mu);

			// Compute unnormalised importance weights
			weights[ii] = logw / probs_grid(ii,0);

			// Save optimised alpha / mu
			alpha_i[ii] = alpha;
			mu_i[ii] = mu;
		}

		// Normalise importance weights
		double my_sum;
		for (int ii = 0; ii < n_grid; ii++){
			my_sum += weights[ii];
		}

		for (int ii = 0; ii < n_grid; ii++){
			weights[ii] /= my_sum;
		}

		// Average alpha + mu over hyperparams
		alpha_av.resize(n_var, 0.0);
		mu_av.resize(n_var, 0.0);
		beta_av.resize(n_var, 0.0);
		for (int ii = 0; ii < n_grid; ii++){
			for (int kk = 0; kk < n_var; kk++){
				alpha_av[kk] += weights[ii] * alpha_i[ii](kk);
				mu_av[kk] += weights[ii] * mu_i[ii](kk);
				beta_av[kk] += weights[ii] * mu_i[ii](kk) * alpha_i[ii](kk);
			}
		}
	}

	void write_to_file( std::string ofile ){
		std::string gz_str = ".gz";
		std::size_t pos = ofile.rfind(".");
		std::string ofile_hyps = ofile.substr(0, --pos) + "_hyps." + ofile.substr(++(++pos), ofile.length());
		std::cout << "Writing posterior PIP and beta probabilities to " << ofile << std::endl;
		std::cout << "Writing posterior hyperparameter probabilities to " << ofile_hyps << std::endl;

		boost_io::filtering_ostream outf;
		boost_io::filtering_ostream outf_hyps;
		if (ofile.find(gz_str) != std::string::npos) {
			outf.push(boost_io::gzip_compressor());
			outf_hyps.push(boost_io::gzip_compressor());
		}
		outf.push(boost_io::file_sink(ofile.c_str()));
		outf_hyps.push(boost_io::file_sink(ofile_hyps.c_str()));

		outf << "post_alpha post_mu post_beta" << std::endl;
		for (int kk = 0; kk < n_var; kk++){
			outf << alpha_av[kk] << " " << mu_av[kk] << std::endl;
		}

		outf_hyps << "post_hyps" << std::endl;
		for (int ii = 0; ii < n_grid; ii++){
			outf_hyps << weights[ii] << std::endl;
		}
	}
};

inline double sigmoid(double x){
	return 1.0 / (1.0 + std::exp(-x));
}

inline double int_klbeta(Eigen::VectorXd alpha,
						 Eigen::VectorXd mu, 
						 std::vector< double > s_sq,
						 double var,
						 int n_var,
						 double eps){
	double res = 0;
	for (int kk = 0; kk < n_var; kk++){
		res += alpha(kk) * (1.0 + std::log(s_sq[kk] / var) -
							(s_sq[kk] + mu(kk) * mu(kk)) / var) / 2.0;

		res -= alpha[kk] * std::log(alpha[kk] + eps);
		res -= (1 - alpha[kk]) * std::log(1 - alpha[kk] + eps);
	}
	return res;
}

#endif
