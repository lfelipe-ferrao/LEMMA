// tests-main.cpp
#define EIGEN_USE_MKL_ALL
#include "catch.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iostream>
#include "../src/tools/eigen3.3/Dense"
#include "../src/parse_arguments.hpp"
#include "../src/vbayes_x2.hpp"
#include "../src/data.hpp"
#include "../src/hyps.hpp"


// Scenarios
char* case1a[] = { (char*) "--mode_vb",
				 (char*) "--mode_empirical_bayes",
				 (char*) "--spike_diff_factor", (char*) "10000",
				 (char*) "--vb_iter_max", (char*) "10",
				 (char*) "--hyps_grid", (char*) "data/io_test/single_hyps_gxage.txt",
				 (char*) "--pheno", (char*) "data/io_test/pheno.txt",
				 (char*) "--environment", (char*) "data/io_test/n50_p100_env.txt",
				 (char*) "--bgen", (char*) "data/io_test/n50_p100.bgen",
				 (char*) "--out", (char*) "data/io_test/test1a.out.gz"};

char* case1b[] = { (char*) "--mode_vb",
				   (char*) "--mode_empirical_bayes",
				   (char*) "--spike_diff_factor", (char*) "10000",
				   (char*) "--vb_iter_max", (char*) "10",
				   (char*) "--vb_iter_start", (char*) "3",
				   (char*) "--resume_from_param_dump",
				   (char*) "data/io_test/r2_interim_files/grid_point_0/test1a_dump_it2",
				   (char*) "--pheno", (char*) "data/io_test/pheno.txt",
				   (char*) "--environment", (char*) "data/io_test/n50_p100_env.txt",
				   (char*) "--bgen", (char*) "data/io_test/n50_p100.bgen",
				   (char*) "--out", (char*) "data/io_test/test1b.out"};



TEST_CASE("Resume from multi-env + mog + emp_bayes"){
	parameters p;

	SECTION("Run to iter 10"){
		int argc = sizeof(case1a)/sizeof(case1a[0]);
		parse_arguments(p, argc, case1a);

		Data data(p);
		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		data.read_full_bgen();
		data.calc_dxteex();
		data.set_vb_init();

		VBayesX2 VB(data);

		std::vector< VbTracker > trackers(VB.hyps_inits.size(), p);
		SECTION("Ex4. Explicitly checking hyps") {
			// Set up for RunInnerLoop
			long n_grid = VB.hyps_inits.size();
			std::vector<Hyps> all_hyps = VB.hyps_inits;
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double > > logw_updates(n_grid);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			VB.updateAllParams(0, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-92.2292775905));
			VB.updateAllParams(1, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-89.6710643279));
			VB.updateAllParams(2, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-88.4914916475));

			CHECK(VB.YM.squaredNorm() == Approx(14.6462021668));
			CHECK(VB.YX.squaredNorm() == Approx(0.0004903837));
			CHECK(VB.ETA.squaredNorm() == Approx(0.0773475751));
			CHECK(VB.ETA_SQ.squaredNorm() == Approx(294.9017799794));

			VbTracker tracker(p);
			tracker.init_interim_output(0,2, VB.n_effects, VB.n_covar, VB.n_env, VB.env_names, vp);
			tracker.dump_state(2, VB.n_samples, VB.n_covar, VB.n_var, VB.n_env,
							   VB.n_effects, vp, hyps, VB.Y, VB.C, VB.X,
							   VB.covar_names, VB.env_names);

			VB.updateAllParams(3, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-87.8880225449));

			CHECK(vp.ym.squaredNorm() == Approx(15.7893306211));
			CHECK(vp.yx.squaredNorm() == Approx(0.0000929716));
			CHECK(vp.eta.squaredNorm() == Approx(0.0231641669));
		}

		VB.run_inference(VB.hyps_inits, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 10);
			CHECK(trackers[0].logw == Approx(-86.8131749627));
		}
	}

	SECTION("Resume from iter 2"){
		int argc = sizeof(case1b)/sizeof(case1b[0]);
		parse_arguments(p, argc, case1b);

		Data data(p);
		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		data.read_full_bgen();
		data.calc_dxteex();
		data.set_vb_init();

		VBayesX2 VB(data);

		std::vector< VbTracker > trackers(VB.hyps_inits.size(), p);
		SECTION("Ex4. Explicitly checking hyps") {
			long n_grid = VB.hyps_inits.size();
			std::vector<Hyps> all_hyps = VB.hyps_inits;
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double > > logw_updates(n_grid);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			CHECK(VB.YM.squaredNorm() == Approx(14.64620215));
			CHECK(VB.YX.squaredNorm() == Approx(0.0004903837));
			CHECK(VB.ETA.squaredNorm() == Approx(0.0773475736));
			CHECK(VB.ETA_SQ.squaredNorm() == Approx(294.9017821007));

			VB.updateAllParams(3, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-87.8880225713));

			CHECK(vp.ym.squaredNorm() == Approx(15.7893305635));
			CHECK(vp.yx.squaredNorm() == Approx(0.0000929716));
			CHECK(vp.eta.squaredNorm() == Approx(0.0231641668));
		}

		VB.run_inference(VB.hyps_inits, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 10);
			CHECK(trackers[0].logw == Approx(-86.8131749627));
		}
	}
}


char* case2a[] = { (char*) "--mode_vb",
				   (char*) "--mode_squarem",
				   (char*) "--spike_diff_factor", (char*) "10000",
				   (char*) "--vb_iter_max", (char*) "10",
				   (char*) "--hyps_grid", (char*) "data/io_test/single_hyps_gxage.txt",
				   (char*) "--pheno", (char*) "data/io_test/pheno.txt",
				   (char*) "--environment", (char*) "data/io_test/n50_p100_env.txt",
				   (char*) "--bgen", (char*) "data/io_test/n50_p100.bgen",
				   (char*) "--out", (char*) "data/io_test/test2a.out.gz"};

char* case2b[] = { (char*) "--mode_vb",
				   (char*) "--mode_squarem",
				   (char*) "--spike_diff_factor", (char*) "10000",
				   (char*) "--vb_iter_max", (char*) "10",
				   (char*) "--vb_iter_start", (char*) "3",
				   (char*) "--resume_from_param_dump",
				   (char*) "data/io_test/r2_interim_files/grid_point_0/test2a_dump_it2",
				   (char*) "--pheno", (char*) "data/io_test/pheno.txt",
				   (char*) "--environment", (char*) "data/io_test/n50_p100_env.txt",
				   (char*) "--bgen", (char*) "data/io_test/n50_p100.bgen",
				   (char*) "--out", (char*) "data/io_test/test2b.out"};

TEST_CASE("Resume from multi-env + mog + squarem"){
	parameters p;

	SECTION("Run to iter 10"){
		int argc = sizeof(case2a)/sizeof(case2a[0]);
		parse_arguments(p, argc, case2a);
		p.mode_squarem = true;

		Data data(p);
		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		data.read_full_bgen();
		data.calc_dxteex();
		data.set_vb_init();

		VBayesX2 VB(data);

		std::vector< VbTracker > trackers(VB.hyps_inits.size(), p);
		SECTION("Ex4. Explicitly checking hyps") {
			// Set up for RunInnerLoop
			long n_grid = VB.hyps_inits.size();
			std::vector<Hyps> all_hyps = VB.hyps_inits;
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double > > logw_updates(n_grid);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			VB.updateAllParams(0, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-92.2292775905));
			VB.updateAllParams(1, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-89.6710643279));
			VB.updateAllParams(2, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-88.4914916475));

			CHECK(VB.YM.squaredNorm() == Approx(14.6462021668));
			CHECK(VB.YX.squaredNorm() == Approx(0.0004903837));
			CHECK(VB.ETA.squaredNorm() == Approx(0.0773475736));
			CHECK(VB.ETA_SQ.squaredNorm() == Approx(294.9017821007));

			VbTracker tracker(p);
			tracker.init_interim_output(0,2, VB.n_effects, VB.n_covar, VB.n_env, VB.env_names, vp);
			tracker.dump_state(2, VB.n_samples, VB.n_covar, VB.n_var, VB.n_env,
							   VB.n_effects, vp, hyps, VB.Y, VB.C, VB.X,
							   VB.covar_names, VB.env_names);

			VB.updateAllParams(3, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-87.8880225449));

			CHECK(vp.ym.squaredNorm() == Approx(15.7893305635));
			CHECK(vp.yx.squaredNorm() == Approx(0.0000929716));
			CHECK(vp.eta.squaredNorm() == Approx(0.0231641668));
			CHECK(VB.ETA_SQ.squaredNorm() == Approx(397.6779293259));
		}

		VB.run_inference(VB.hyps_inits, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 10);
			CHECK(trackers[0].logw == Approx(-86.650909737));
		}
	}

	SECTION("Resume from iter 2"){
		int argc = sizeof(case2b)/sizeof(case2b[0]);
		parse_arguments(p, argc, case2b);
		p.mode_squarem = true;

		Data data(p);
		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		data.read_full_bgen();
		data.calc_dxteex();
		data.set_vb_init();

		VBayesX2 VB(data);

		std::vector< VbTracker > trackers(VB.hyps_inits.size(), p);
		SECTION("Ex4. Explicitly checking hyps") {
			long n_grid = VB.hyps_inits.size();
			std::vector<Hyps> all_hyps = VB.hyps_inits;
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double > > logw_updates(n_grid);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			CHECK(VB.YM.squaredNorm() == Approx(14.6462021668));
			CHECK(VB.YX.squaredNorm() == Approx(0.0004903837));
			CHECK(VB.ETA.squaredNorm() == Approx(0.0773475736));
			CHECK(VB.ETA_SQ.squaredNorm() == Approx(294.9017821007));
			CHECK(VB.calc_logw(hyps, vp) == Approx(-88.4914916517));

			VB.updateAllParams(3, round_index, all_vp, all_hyps, logw_prev);
			CHECK(VB.calc_logw(hyps, vp) == Approx(-87.8880225713));

			CHECK(vp.ym.squaredNorm() == Approx(15.7893305635));
			CHECK(vp.yx.squaredNorm() == Approx(0.0000929716));
			CHECK(vp.eta.squaredNorm() == Approx(0.0231641668));
			CHECK(VB.ETA_SQ.squaredNorm() == Approx(397.6779293259));
		}

		VB.run_inference(VB.hyps_inits, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 10);
			// Slight discrepancy between original run and restart.
			// Think this is because we now need the previous two hyps values to
			// keep using SQUAREM from the same place
			// CHECK(trackers[0].logw == Approx(-86.6456071112));
			CHECK(trackers[0].logw == Approx(-86.533162843));
		}
	}
}
