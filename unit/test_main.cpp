// tests-main.cpp
#define EIGEN_USE_MKL_ALL
#include "catch.hpp"


#include <algorithm>
#include <cmath>
#include <limits>
#include <iostream>
#include <sys/stat.h>
#include "../src/tools/eigen3.3/Dense"
#include "../src/parse_arguments.hpp"
#include "../src/vbayes_x2.hpp"
#include "../src/data.hpp"
#include "../src/hyps.hpp"
#include "../src/genotype_matrix.hpp"


TEST_CASE( "Algebra in Eigen3" ) {

	Eigen::MatrixXd X(3, 3), X2;
	Eigen::VectorXd v1(3), v2(3);
	X << 1, 2, 3,
		 4, 5, 6,
		 7, 8, 9;
	v1 << 1, 1, 1;
	v2 << 1, 2, 3;
	X2 = X.rowwise().reverse();

	SECTION("dot product of vector with col vector"){
		CHECK((v1.dot(X.col(0))) == 12.0);
	}

	SECTION("Eigen reverses columns as expected"){
		Eigen::MatrixXd res(3, 3);
		res << 3, 2, 1,
			   6, 5, 4,
			   9, 8, 7;
		CHECK(X2 == res);
	}

	SECTION("coefficient-wise product between vectors"){
		Eigen::VectorXd res(3);
		res << 1, 2, 3;
		CHECK((v1.array() * v2.array()).matrix() == res);
		CHECK(v1.cwiseProduct(v2) == res);
	}

	SECTION("coefficient-wise subtraction between vectors"){
		Eigen::VectorXd res(3);
		res << 0, 1, 2;
		CHECK((v2 - v1) == res);
	}

	SECTION("Check .sum() function"){
		Eigen::VectorXd res(3);
		res << 1, 2, 3;
		CHECK(res.sum() == 6);
	}

	SECTION("Sum of NaN returns NaN"){
		Eigen::VectorXd res(3);
		res << 1, std::numeric_limits<double>::quiet_NaN(), 3;
		CHECK(std::isnan(res.sum()));
	}

	SECTION("Ref of columns working correctly"){
		Eigen::Ref<Eigen::VectorXd> y1 = X.col(0);
		CHECK(y1(0) == 1);
		CHECK(y1(1) == 4);
		CHECK(y1(2) == 7);
		X = X + X;
		CHECK(y1(0) == 2);
		CHECK(y1(1) == 8);
		CHECK(y1(2) == 14);
	}

	SECTION("Conservative Resize"){
		std::vector<int> keep;
		keep.push_back(1);
		for (std::size_t i = 0; i < keep.size(); i++) {
			X.col(i) = X.col(keep[i]);
		}
		X.conservativeResize(X.rows(), keep.size());

		CHECK(X.rows() == 3);
		CHECK(X.cols() == 1);
		CHECK(X(0, 0) == 2);
	}

	SECTION("selfAdjoit views"){
		Eigen::MatrixXd m3(3, 3);
		m3.triangularView<Eigen::StrictlyUpper>() = X.transpose() * X;
		CHECK(m3(0, 1) == 78);
	}

	SECTION("colwise subtraction between vector and matrix"){
		Eigen::MatrixXd res;
		res = -1*(X.colwise() - v1);
		CHECK(res(0, 0) == 0);
		CHECK(res.rows() == 3);
		CHECK(res.cols() == 3);
	}
}

TEST_CASE("Data") {
	parameters p;

	p.env_file = "data/io_test/n50_p100_env.txt";
	p.pheno_file = "data/io_test/pheno.txt";

	SECTION("n50_p100.bgen (low mem) w covars") {
		p.covar_file = "data/io_test/age.txt";
		p.bgen_file = "data/io_test/n50_p100.bgen";
		p.bgi_file = "data/io_test/n50_p100.bgen.bgi";
		p.low_mem = true;
		Data data(p);

		data.read_non_genetic_data();
		CHECK(data.n_env == 4);
		CHECK(data.E(0, 0) == Approx(0.785198212));

		data.standardise_non_genetic_data();
		CHECK(data.params.use_vb_on_covars);
		CHECK(data.E(0, 0) == Approx(0.9959851422));

		data.read_full_bgen();
		SECTION("Ex1. bgen read in & standardised correctly") {
			CHECK(data.G.low_mem);
			CHECK(data.params.low_mem);
			CHECK(!data.params.flip_high_maf_variants);
			CHECK(data.G(0, 0) == Approx(-1.8575040711));
			CHECK(data.G(0, 1) == Approx(-0.7404793547));
			CHECK(data.G(0, 2) == Approx(-0.5845122102));
			CHECK(data.G(0, 3) == Approx(-0.6633007506));
			CHECK(data.n_var == 67);
		}

		SECTION("dXtEEX computed correctly") {
			data.calc_dxteex();
			CHECK(data.dXtEEX(0, 0) == Approx(42.2994405499));
			CHECK(data.dXtEEX(1, 0) == Approx(43.2979303929));
			CHECK(data.dXtEEX(2, 0) == Approx(37.6440444004));
			CHECK(data.dXtEEX(3, 0) == Approx(40.9258647207));

			CHECK(data.dXtEEX(0, 4) == Approx(-4.0453940676));
			CHECK(data.dXtEEX(1, 4) == Approx(-15.6140263169));
			CHECK(data.dXtEEX(2, 4) == Approx(-13.2508795732));
			CHECK(data.dXtEEX(3, 4) == Approx(-9.8081456731));
		}
	}

	SECTION("n50_p100.bgen (low mem), covars, sample subset") {
		p.covar_file = "data/io_test/age.txt";
		p.bgen_file = "data/io_test/n50_p100.bgen";
		p.bgi_file = "data/io_test/n50_p100.bgen.bgi";
		p.incl_sids_file = "data/io_test/sample_ids.txt";
		p.low_mem = true;
		Data data(p);

		data.read_non_genetic_data();
		CHECK(data.n_env == 4);
		CHECK(data.E(0, 0) == Approx(0.785198212));

		data.standardise_non_genetic_data();
		CHECK(data.params.use_vb_on_covars);
		CHECK(data.E(0, 0) == Approx(0.8123860763));

		data.read_full_bgen();

		SECTION("dXtEEX computed correctly") {
			data.calc_dxteex();
			CHECK(data.dXtEEX(0, 0) == Approx(23.2334219303));
			CHECK(data.dXtEEX(1, 0) == Approx(27.9920667408));
			CHECK(data.dXtEEX(2, 0) == Approx(24.7041225993));
			CHECK(data.dXtEEX(3, 0) == Approx(24.2423580715));

			CHECK(data.dXtEEX(0, 4) == Approx(-1.056112897));
			CHECK(data.dXtEEX(1, 4) == Approx(-8.526431457));
			CHECK(data.dXtEEX(2, 4) == Approx(-6.5950206611));
			CHECK(data.dXtEEX(3, 4) == Approx(-3.6842212598));
		}
	}

	SECTION("n50_p100.bgen (low mem) + non genetic data") {
		p.bgen_file = "data/io_test/n50_p100.bgen";
		p.bgi_file = "data/io_test/n50_p100.bgen.bgi";
		p.low_mem = true;
		Data data(p);

		data.read_non_genetic_data();
		SECTION("Ex1. Raw non genetic data read in accurately") {
			CHECK(data.n_env == 4);
			CHECK(data.n_pheno == 1);
			CHECK(data.n_samples == 50);
			CHECK(data.Y(0, 0) == Approx(-1.18865038973338));
			CHECK(data.E(0, 0) == Approx(0.785198212));
		}
//
		data.standardise_non_genetic_data();
		SECTION("Check non genetic data standardised + covars regressed") {
			CHECK(data.params.scale_pheno);
			CHECK(data.params.use_vb_on_covars);
			CHECK(data.params.covar_file == "NULL");
//			CHECK(data.Y(0,0) == Approx(-3.6676363273605137)); Centered
			CHECK(data.Y(0,0) == Approx(-1.5800573524786081));
			CHECK(data.Y2(0, 0) == Approx(-1.5567970303));
			CHECK(data.E(0, 0) == Approx(0.8957059881));
		}

		data.read_full_bgen();
		SECTION("Ex1. bgen read in & standardised correctly") {
			CHECK(data.G.low_mem);
			CHECK(data.params.low_mem);
			CHECK(!data.params.flip_high_maf_variants);
			CHECK(data.G(0, 0) == Approx(-1.8575040711));
			CHECK(data.G(0, 1) == Approx(-0.7404793547));
			CHECK(data.G(0, 2) == Approx(-0.5845122102));
			CHECK(data.G(0, 3) == Approx(-0.6633007506));
			CHECK(data.n_var == 67);
		}

		SECTION("dXtEEX computed correctly") {
			data.calc_dxteex();
			CHECK(data.dXtEEX(0, 0) == Approx(38.9610805993));
			CHECK(data.dXtEEX(1, 0) == Approx(38.2995451744));
			CHECK(data.dXtEEX(2, 0) == Approx(33.7077899144));
			CHECK(data.dXtEEX(3, 0) == Approx(35.7391671158));

			CHECK(data.dXtEEX(0, 4) == Approx(-2.6239467101));
			CHECK(data.dXtEEX(1, 4) == Approx(-13.0001255314));
			CHECK(data.dXtEEX(2, 4) == Approx(-11.6635557299));
			CHECK(data.dXtEEX(3, 4) == Approx(-7.2154836264));
		}

		SECTION("Ex1. Confirm calc_dxteex() reorders properly") {
			data.params.dxteex_file = "data/io_test/case8/dxteex_low_mem.txt";
			data.read_external_dxteex();
			data.calc_dxteex();
			CHECK(data.dXtEEX(0, 0) == Approx(38.9610805993));
			CHECK(data.dXtEEX(1, 0) == Approx(38.2995451744));
			CHECK(data.dXtEEX(2, 0) == Approx(33.7077899144));
			CHECK(data.dXtEEX(3, 0) == Approx(35.7391671158));

			CHECK(data.dXtEEX(0, 4) == Approx(-2.6239467101));
			CHECK(data.dXtEEX(1, 4) == Approx(-13.0001255314));
			CHECK(data.dXtEEX(2, 4) == Approx(-11.6635557299));
			CHECK(data.dXtEEX(3, 4) == Approx(-7.2154836264));
			CHECK(data.n_dxteex_computed == 75);
		}
	}

	SECTION("n50_p100_chr2.bgen") {
		p.bgen_file = "data/io_test/n50_p100_chr2.bgen";
		p.bgi_file = "data/io_test/n50_p100_chr2.bgen.bgi";
		Data data(p);

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		data.read_full_bgen();
		SECTION("Ex1. bgen read in & standardised correctly") {
			CHECK(data.G.low_mem);
			CHECK(data.params.low_mem);
			CHECK(!data.params.flip_high_maf_variants);
			CHECK(data.G(0, 0) == Approx(0.7105269065));
			CHECK(data.G(0, 1) == Approx(0.6480740698));
			CHECK(data.G(0, 2) == Approx(0.7105195023));
			CHECK(data.G(0, 3) == Approx(-0.586791551));
			CHECK(data.G(0, 60) == Approx(-1.4317770638));
			CHECK(data.G(0, 61) == Approx(1.4862052498));
			CHECK(data.G(0, 62) == Approx(-0.3299831646));
			CHECK(data.G(0, 63) == Approx(-1.0968694989));
			CHECK(data.G.compressed_dosage_means(60) == Approx(1.00203125));
			CHECK(data.G.compressed_dosage_means(61) == Approx(0.9821875));
			CHECK(data.G.compressed_dosage_means(62) == Approx(0.10390625));
			CHECK(data.G.compressed_dosage_means(63) == Approx(0.68328125));
			CHECK(data.n_var == 75);
		}
	}

	SECTION("n50_p100_chr2.bgen w/ 2 chunks") {
		p.bgen_file = "data/io_test/n50_p100_chr2.bgen";
		p.bgi_file = "data/io_test/n50_p100_chr2.bgen.bgi";
		p.chunk_size = 72;
		p.n_bgen_thread = 2;
		Data data(p);

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		data.read_full_bgen();
		SECTION("Ex1. bgen read in & standardised correctly") {
			CHECK(data.G.low_mem);
			CHECK(data.params.low_mem);
			CHECK(data.params.flip_high_maf_variants);
			CHECK(data.G(0, 0) == Approx(-0.7105269065));
			CHECK(data.G(0, 1) == Approx(-0.6480740698));
			CHECK(data.G(0, 2) == Approx(-0.7105104917));
			CHECK(data.G(0, 3) == Approx(-0.586791551));
			CHECK(data.G(0, 60) == Approx(1.4862052498));
			CHECK(data.G(0, 61) == Approx(-0.3299831646));
			CHECK(data.G(0, 62) == Approx(-1.0968694989));
			CHECK(data.G(0, 63) == Approx(-0.5227553607));
			CHECK(data.G.compressed_dosage_means(60) == Approx(0.9821875));
			CHECK(data.G.compressed_dosage_means(61) == Approx(0.10390625));
			CHECK(data.G.compressed_dosage_means(62) == Approx(0.68328125));
			CHECK(data.G.compressed_dosage_means(63) == Approx(0.28359375));
			CHECK(data.n_var == 73);
		}
	}

	SECTION("Check mult_vector_by_chr"){
		p.bgen_file = "data/io_test/n50_p100_chr2.bgen";
		p.bgi_file = "data/io_test/n50_p100_chr2.bgen.bgi";
		Data data(p);

		data.read_non_genetic_data();
		data.read_full_bgen();

		Eigen::VectorXd vv = Eigen::VectorXd::Ones(data.G.pp);
		Eigen::VectorXd v1 = data.G.mult_vector_by_chr(1, vv);
		Eigen::VectorXd v2 = data.G.mult_vector_by_chr(22, vv);

		CHECK(v1(0) == Approx(-9.6711528276));
		CHECK(v1(1) == Approx(-0.4207388213));
		CHECK(v1(2) == Approx(-3.0495872499));
		CHECK(v1(3) == Approx(-9.1478619829));

		CHECK(v2(0) == Approx(-15.6533077013));
		CHECK(v2(1) == Approx(6.8078348334));
		CHECK(v2(2) == Approx(-4.4887853578));
		CHECK(v2(3) == Approx(8.9980192447));
	}
}

TEST_CASE( "Example 1: single-env" ){
	parameters p;

	SECTION("Ex1. No filters applied, low mem mode"){
		char* argv[] = { (char*) "bin/bgen_prog", (char*) "--mode_vb", (char*) "--low_mem",
						 (char*) "--mode_spike_slab", (char*) "--mode_regress_out_covars",
						 (char*) "--bgen", (char*) "data/io_test/n50_p100.bgen",
						 (char*) "--out", (char*) "data/io_test/fake_age.out",
						 (char*) "--pheno", (char*) "data/io_test/pheno.txt",
						 (char*) "--hyps_grid", (char*) "data/io_test/hyperpriors_gxage.txt",
						 (char*) "--hyps_probs", (char*) "data/io_test/hyperpriors_gxage_probs.txt",
						 (char*) "--vb_init", (char*) "data/io_test/answer_init.txt",
						 (char*) "--environment", (char*) "data/io_test/age.txt"};
		int argc = sizeof(argv)/sizeof(argv[0]);
		parse_arguments(p, argc, argv);
		Data data( p );

		std::cout << "Data initialised" << std::endl;
		data.read_non_genetic_data();
		SECTION( "Ex1. Raw non genetic data read in accurately"){
//            CHECK(data.n_covar == 1);
            CHECK(data.n_env == 1);
			CHECK(data.n_pheno == 1);
			CHECK(data.n_samples == 50);
			CHECK(data.Y(0,0) == Approx(-1.18865038973338));
			//CHECK(data.W(0,0) == Approx(-0.33472645347487201));
			CHECK(data.E(0, 0) == Approx(-0.33472645347487201));
			CHECK(data.hyps_grid(0,1) == Approx(0.317067781333932));
		}

		data.standardise_non_genetic_data();
		SECTION( "Ex1. Non genetic data standardised + covars regressed"){
			CHECK(data.params.scale_pheno == true);
			CHECK(data.params.use_vb_on_covars == false);
			CHECK(data.params.covar_file == "NULL");
//			CHECK(data.Y(0,0) == Approx(-3.6676363273605137)); Centered
//			CHECK(data.Y(0,0) == Approx(-1.5800573524786081)); Scaled
			CHECK(data.Y(0,0) == Approx(-1.262491384814441));
			CHECK(data.Y2(0,0) == Approx(-1.262491384814441));
//			CHECK(data.W(0,0) == Approx(-0.58947939694779772));
			CHECK(data.E(0,0) == Approx(-0.58947939694779772));
		}

		data.read_full_bgen();
		SECTION( "Ex1. bgen read in & standardised correctly"){
			CHECK(data.G.low_mem);
			CHECK(data.params.low_mem);
            CHECK(data.params.flip_high_maf_variants);
			CHECK(data.G(0, 0) == Approx(1.8570984229));
		}

		SECTION( "Ex1. Confirm calc_dxteex() reorders properly"){
		    data.params.dxteex_file = "data/io_test/inputs/dxteex_mixed.txt";
			data.read_external_dxteex();
            data.calc_dxteex();
            CHECK(data.dXtEEX(0, 0) == Approx(87.204591182113916));
            CHECK(data.n_dxteex_computed == 1);
		}

		data.calc_dxteex();
		if(p.vb_init_file != "NULL"){
			data.read_alpha_mu();
		}
		VBayesX2 VB(data);
		VB.check_inputs();
		SECTION("Ex1. Vbayes_X2 initialised correctly"){
			CHECK(VB.n_samples == 50);
			CHECK(VB.N == 50.0);
			CHECK(VB.n_env == 1);
//			CHECK(VB.n_covar == 1);
			CHECK(VB.n_effects == 2);
			CHECK(VB.vp_init.muw(0) == 1.0);
			CHECK(!VB.p.init_weights_with_snpwise_scan);
			CHECK(VB.dXtEEX(0, 0) == Approx(87.1907593967));
		}

		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
        SECTION("Ex1. Explicitly checking updates"){
			// Initialisation
#ifdef DATA_AS_FLOAT
			CHECK( (double)  VB.vp_init.ym(0) == Approx(0.0003200434));
#else
			CHECK(VB.vp_init.ym(0) == Approx(0.0003200476));
#endif
			CHECK(VB.vp_init.yx(0) == Approx(0.0081544079));
			CHECK(VB.vp_init.eta(0) == Approx(-0.5894793969));

			// Set up for RunInnerLoop
			long n_grid = VB.hyps_grid.rows();
			long n_samples = VB.n_samples;
			std::vector<Hyps> all_hyps;
			VB.unpack_hyps(VB.hyps_grid, all_hyps);

			// Set up for updateAllParams
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double >> logw_updates(n_grid);

			// Ground zero as expected
			CHECK(vp.alpha_beta(0) * vp.mu1_beta(0) == Approx(-0.00015854116408000002));
			CHECK(data.Y(0,0) == Approx(-1.262491384814441));
#ifdef DATA_AS_FLOAT
			CHECK( (double) vp.ym(0) == Approx( 0.0003200434));
#else
			CHECK(vp.ym(0) == Approx(0.0003200476));
#endif
			CHECK(vp.yx(0) == Approx(0.0081544079));
			CHECK(vp.eta(0) == Approx(-0.5894793969));

			VB.updateAllParams(0, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(VB.X.col(0)(0) == Approx(1.8570984229));
			CHECK(vp.s1_beta_sq(0) == Approx(0.0031087381));
			CHECK(vp.mu1_beta(0) == Approx(-0.0303900712));
			CHECK(vp.alpha_beta(0) == Approx(0.1447783263));
			CHECK(vp.alpha_beta(1) == Approx(0.1517251004));
			CHECK(vp.mu1_beta(1) == Approx(-0.0355760798));
			CHECK(vp.alpha_beta(63) == Approx(0.1784518373));
			CHECK(VB.calc_logw(hyps, vp) == Approx(-60.983398393));

			VB.updateAllParams(1, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(vp.alpha_beta(0) == Approx(0.1350711123));
			CHECK(vp.mu1_beta(0) == Approx(-0.0205395866));
			CHECK(vp.alpha_beta(1) == Approx(0.1400764528));
			CHECK(vp.alpha_beta(63) == Approx(0.1769882239));
			CHECK(VB.calc_logw(hyps, vp) == Approx(-60.606081598));
		}

		VB.run_inference(VB.hyps_grid, false, 2, trackers);
		SECTION("Ex1. Vbayes_X2 inference correct") {
			CHECK(trackers[0].count == 33);
			CHECK(trackers[3].count == 33);
			CHECK(trackers[0].logw == Approx(-60.522210486));
			CHECK(trackers[1].logw == Approx(-59.9696083263));
			CHECK(trackers[2].logw == Approx(-60.30658117));
			CHECK(trackers[3].logw == Approx(-61.0687573393));
		}
	}
}

TEST_CASE( "Example 2a: multi-env + bgen over 2chr" ){
	parameters p;

	SECTION("Ex2. No filters applied, high mem mode"){
		char* argv[] = { (char*) "bin/bgen_prog", (char*) "--mode_vb", (char*) "--high_mem",
						 (char*) "--mode_spike_slab", (char*) "--mode_regress_out_covars",
						 (char*) "--environment", (char*) "data/io_test/n50_p100_env.txt",
						 (char*) "--bgen", (char*) "data/io_test/n50_p100_chr2.bgen",
						 (char*) "--out", (char*) "data/io_test/fake_env.out",
						 (char*) "--pheno", (char*) "data/io_test/pheno.txt",
						 (char*) "--hyps_grid", (char*) "data/io_test/hyperpriors_gxage.txt",
						 (char*) "--vb_init", (char*) "data/io_test/answer_init.txt"};
		int argc = sizeof(argv)/sizeof(argv[0]);
		parse_arguments(p, argc, argv);
		Data data( p );

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		data.read_full_bgen();

		data.calc_dxteex();
        data.calc_snpstats();
		if(p.vb_init_file != "NULL"){
			data.read_alpha_mu();
		}
		VBayesX2 VB(data);
		VB.check_inputs();
		SECTION("Ex2. Vbayes_X2 initialised correctly"){
			CHECK(VB.n_samples == 50);
			CHECK(VB.N == 50.0);
			CHECK(VB.n_var == 73);
			CHECK(VB.n_env == 4);
			// CHECK(VB.n_covar == 4);
			//CHECK(VB.n_effects == 2);
			CHECK(VB.vp_init.muw(0) == 0.25);
			CHECK(VB.p.init_weights_with_snpwise_scan == false);
			CHECK(VB.dXtEEX(0, 0) == Approx(44.6629676819));
		}

		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
		SECTION("Ex2. Explicitly checking updates"){
			// Set up for RunInnerLoop
			long n_grid = VB.hyps_grid.rows();
			long n_samples = VB.n_samples;
			std::vector<Hyps> all_hyps;
			VB.unpack_hyps(VB.hyps_grid, all_hyps);

			// Set up for updateAllParams
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double >> logw_updates(n_grid);

			VB.updateAllParams(0, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(vp.alpha_beta(0) == Approx(0.0103168718));
			CHECK(vp.alpha_beta(1) == Approx(0.0101560491));
			CHECK(vp.alpha_beta(63) == Approx(0.0098492375));
			CHECK(vp.alpha_gam(0) == Approx(0.013394603));
			CHECK(vp.muw(0) == Approx(0.1593944543));
			CHECK(VB.calc_logw(hyps, vp) == Approx(-71.1292851018));

			VB.updateAllParams(1, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(vp.alpha_beta(0) == Approx(0.0101823562));
			CHECK(vp.alpha_beta(1) == Approx(0.0100615294));
			CHECK(vp.alpha_beta(63) == Approx(0.0098486026));
			CHECK(vp.muw(0) == Approx(0.031997336));
			CHECK(VB.calc_logw(hyps, vp) == Approx(-69.8529334166));
		}

		VB.run_inference(VB.hyps_grid, false, 2, trackers);
		SECTION("Ex2. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 10);
			CHECK(trackers[3].count == 10);
			CHECK(trackers[0].logw == Approx(-69.7419880272));
			CHECK(trackers[1].logw == Approx(-69.9470990972));
			CHECK(trackers[2].logw == Approx(-70.1298787803));
			CHECK(trackers[3].logw == Approx(-70.2928879787));
		}

		SECTION("Partition residuals amongst chromosomes"){
			int n_chrs = VB.n_chrs;
			long n_samples = VB.n_samples;
			std::vector<Eigen::VectorXd> map_residuals_by_chr(n_chrs), pred_main(n_chrs), pred_int(n_chrs);
			long ii_map = 0;
			VariationalParametersLite vp_map = trackers[ii_map].vp;

			// Predicted effects to file
			VB.calcPredEffects(vp_map);
			VB.compute_residuals_per_chr(vp_map, pred_main, pred_int, map_residuals_by_chr);

			Eigen::VectorXd check_Xgam = Eigen::VectorXd::Zero(n_samples);
			Eigen::VectorXd check_Xbeta = Eigen::VectorXd::Zero(n_samples);
			Eigen::VectorXd check_resid = Eigen::VectorXd::Zero(n_samples);
			for (int cc = 0; cc < n_chrs; cc++){
				check_Xbeta += pred_main[cc];
				check_Xgam += pred_int[cc];
				check_resid += map_residuals_by_chr[cc];
			}
			if(VB.p.use_vb_on_covars) {
				check_Xbeta += (VB.C * vp_map.muc.matrix().cast<scalarData>()).cast<double>();
				check_resid += (VB.C * vp_map.muc.matrix().cast<scalarData>()).cast<double>();
			}
			check_resid -= VB.Y.cast<double>();
			check_resid /= (double) (n_chrs-1);

			Eigen::VectorXd resid = (VB.Y - vp_map.ym - vp_map.yx.cwiseProduct(vp_map.eta)).cast<double>();

			CHECK(n_chrs == 2);
			CHECK(pred_main[0](0) == Approx(0.0275588533));
			CHECK(pred_main[1](0) == Approx(-0.0404733278));
			CHECK(check_Xbeta(0)  == Approx(vp_map.ym(0)));
			CHECK(check_Xgam(0)   == Approx(vp_map.yx(0)));
			CHECK(check_resid(0)  == Approx(resid(0)));
		}
	}
}

TEST_CASE( "Example 3: multi-env w/ covars" ){
	parameters p;

	SECTION("Ex3. No filters applied, high mem mode"){
		char* argv[] = { (char*) "bin/bgen_prog", (char*) "--mode_vb", (char*) "--high_mem",
						 (char*) "--use_vb_on_covars", (char*) "--mode_spike_slab",
						 (char*) "--environment", (char*) "data/io_test/n50_p100_env.txt.gz",
						 (char*) "--bgen", (char*) "data/io_test/n50_p100.bgen",
						 (char*) "--out", (char*) "data/io_test/fake_env.out",
						 (char*) "--pheno", (char*) "data/io_test/pheno.txt",
						 (char*) "--hyps_grid", (char*) "data/io_test/hyperpriors_gxage.txt",
						 (char*) "--vb_init", (char*) "data/io_test/answer_init.txt"};
		int argc = sizeof(argv)/sizeof(argv[0]);
		parse_arguments(p, argc, argv);
		Data data( p );

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		SECTION( "Ex3. Non genetic data standardised + covars regressed"){
			CHECK(data.params.scale_pheno == true);
			CHECK(data.params.use_vb_on_covars == true);
			CHECK(data.params.covar_file == "NULL");
//			CHECK(data.Y(0,0) == Approx(-3.6676363273605137)); Centered
			CHECK(data.Y(0,0) == Approx(-1.5800573524786081)); // Scaled
			CHECK(data.Y2(0,0) == Approx(-1.5567970303));
			//CHECK(data.W(0,0) == Approx(0.8957059881));
			CHECK(data.E(0,0) == Approx(0.8957059881));
		}
		data.read_full_bgen();

		data.calc_dxteex();
		data.calc_snpstats();
		if(p.vb_init_file != "NULL"){
			data.read_alpha_mu();
		}
		VBayesX2 VB(data);
		VB.check_inputs();
		SECTION("Ex3. Vbayes_X2 initialised correctly"){
			CHECK(VB.n_samples == 50);
			CHECK(VB.N == 50.0);
			CHECK(VB.n_env == 4);
			//CHECK(VB.n_covar == 4);
			CHECK(VB.n_effects == 2);
			CHECK(VB.vp_init.muw(0) == 0.25);
			CHECK(VB.p.init_weights_with_snpwise_scan == false);
			CHECK(VB.dXtEEX(0, 0) == Approx(38.9390135703));
			CHECK(VB.dXtEEX(1, 0) == Approx(38.34695));
			CHECK(VB.dXtEEX(2, 0) == Approx(33.7626));
			CHECK(VB.dXtEEX(3, 0) == Approx(35.71962));

			CHECK(VB.dXtEEX(0, 4) == Approx(-2.58481));
			CHECK(VB.dXtEEX(1, 4) == Approx(-13.04073));
			CHECK(VB.dXtEEX(2, 4) == Approx(-11.69077));
			CHECK(VB.dXtEEX(3, 4) == Approx(-7.17068));
		}

		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
		SECTION("Ex3. Explicitly checking updates"){
			// Set up for RunInnerLoop
			long n_grid = VB.hyps_grid.rows();
			std::vector<Hyps> all_hyps;
			VB.unpack_hyps(VB.hyps_grid, all_hyps);

			// Set up for updateAllParams
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double >> logw_updates(n_grid);

			VB.updateAllParams(0, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			CHECK(vp.muc(0) == Approx(0.1221946024));
			CHECK(vp.muc(3) == Approx(-0.1595909887));
			CHECK(vp.alpha_beta(0) == Approx(0.1339235799));
			CHECK(vp.alpha_beta(1) == Approx(0.1415361555));
			CHECK(vp.alpha_beta(63) == Approx(0.1724736345));
			CHECK(vp.muw(0, 0) == Approx(0.1127445891));
			CHECK(VB.calc_logw(all_hyps[0], vp) == Approx(-94.4656200443));

			CHECK(vp.alpha_gam(0) == Approx(0.1348765515));
			CHECK(vp.alpha_gam(1) == Approx(0.1348843768));
			CHECK(vp.alpha_gam(63) == Approx(0.1351395247));
			CHECK(vp.mu1_beta(0) == Approx(-0.0189890299));
			CHECK(vp.mu1_beta(1) == Approx(-0.0275538256));
			CHECK(vp.mu1_beta(63) == Approx(-0.0470801956));
			CHECK(vp.mu1_gam(0) == Approx(0.0048445126));
			CHECK(vp.mu1_gam(1) == Approx(0.0005509309));
			CHECK(vp.mu1_gam(63) == Approx(-0.0040966814));
			CHECK(vp.s1_gam_sq(0) == Approx(0.0035251837));
			CHECK(vp.s1_gam_sq(1) == Approx(0.0035489038));
			CHECK(vp.s1_gam_sq(63) == Approx(0.0035479273));

			VB.updateAllParams(1, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(vp.muc(0) == Approx(0.1463805515));
			CHECK(vp.muc(3) == Approx(-0.1128544804));
			CHECK(vp.alpha_beta(0) == Approx(0.1292056073));
			CHECK(vp.alpha_beta(1) == Approx(0.1338797264));
			CHECK(vp.alpha_beta(63) == Approx(0.1730150924));
			CHECK(vp.muw(0, 0) == Approx(0.0460748751));
			CHECK(VB.calc_logw(all_hyps[0], vp) == Approx(-93.7888239338));


			CHECK(vp.alpha_gam(0) == Approx(0.1228414938));
			CHECK(vp.alpha_gam(1) == Approx(0.1244760462));
			CHECK(vp.alpha_gam(63) == Approx(0.1240336666));
			CHECK(vp.mu1_gam(0) == Approx(-0.0013406961));
			CHECK(vp.mu1_gam(1) == Approx(-0.0021107307));
			CHECK(vp.mu1_gam(63) == Approx(0.0010160659));
			CHECK(vp.s1_gam_sq(0) == Approx(0.0028616572));
			CHECK(vp.s1_gam_sq(1) == Approx(0.0029466955));
			CHECK(vp.s1_gam_sq(63) == Approx(0.0029262235));

			VB.updateAllParams(2, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(vp.alpha_beta(0) == Approx(0.1291159583));
			CHECK(vp.alpha_beta(1) == Approx(0.1337078986));
			CHECK(vp.alpha_beta(63) == Approx(0.1846784602));
			CHECK(vp.alpha_gam(0) == Approx(0.1205867018));
			CHECK(vp.alpha_gam(1) == Approx(0.1223799879));
			CHECK(vp.alpha_gam(63) == Approx(0.1219421923));
			CHECK(vp.mu1_beta(0) == Approx(-0.0099430405));
			CHECK(vp.mu1_beta(1) == Approx(-0.0186819136));
			CHECK(vp.mu1_beta(63) == Approx(-0.0522879252));
			CHECK(vp.mu1_gam(0) == Approx(-0.0010801898));
			CHECK(vp.mu1_gam(1) == Approx(-0.0010635764));
			CHECK(vp.mu1_gam(63) == Approx(-0.0006202975));
			CHECK(vp.muw(0, 0) == Approx(0.0285866235));
		}

		VB.run_inference(VB.hyps_grid, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 33);
			CHECK(trackers[3].count == 33);
			CHECK(trackers[0].logw == Approx(-93.7003814019));
			CHECK(trackers[1].logw == Approx(-93.3247434264));
			CHECK(trackers[2].logw == Approx(-93.6548417528));
			CHECK(trackers[3].logw == Approx(-94.3511347264));
		}
	}
}

TEST_CASE( "Example 4: multi-env + mog + covars + emp_bayes" ){
	parameters p;

	SECTION("Ex4. No filters applied, high mem mode"){
		char* argv[] = { (char*) "bin/bgen_prog", (char*) "--mode_vb", (char*) "--low_mem",
						 (char*) "--use_vb_on_covars", (char*) "--mode_empirical_bayes",
						 (char*) "--effects_prior_mog",
						 (char*) "--vb_iter_max", (char*) "10",
						 (char*) "--environment", (char*) "data/io_test/n50_p100_env.txt",
						 (char*) "--bgen", (char*) "data/io_test/n50_p100.bgen",
						 (char*) "--out", (char*) "data/io_test/config4.out",
						 (char*) "--pheno", (char*) "data/io_test/pheno.txt",
						 (char*) "--hyps_grid", (char*) "data/io_test/single_hyps_gxage.txt",
						 (char*) "--hyps_probs", (char*) "data/io_test/single_hyps_gxage_probs.txt",
						 (char*) "--vb_init", (char*) "data/io_test/answer_init.txt"};
		int argc = sizeof(argv)/sizeof(argv[0]);
		parse_arguments(p, argc, argv);
		Data data( p );

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		SECTION( "Ex4. Non genetic data standardised + covars regressed"){
			CHECK(data.params.scale_pheno);
			CHECK(data.params.use_vb_on_covars);
			CHECK(data.params.covar_file == "NULL");
//			CHECK(data.Y(0,0) == Approx(-3.6676363273605137)); Centered
			CHECK(data.Y(0,0) == Approx(-1.5800573524786081)); // Scaled
			CHECK(data.Y2(0,0) == Approx(-1.5567970303));
//			CHECK(data.W(0,0) == Approx(0.8957059881));
			CHECK(data.E(0,0) == Approx(0.8957059881));
			CHECK(data.E.row(0).array().sum() == Approx(2.9708148667));
		}
		data.read_full_bgen();

		data.calc_dxteex();
		data.calc_snpstats();
		if(p.vb_init_file != "NULL"){
			data.read_alpha_mu();
		}
		VBayesX2 VB(data);
		VB.check_inputs();
		SECTION("Ex4. Vbayes_X2 initialised correctly"){
			CHECK(VB.n_samples == 50);
			CHECK(VB.N == 50.0);
			CHECK(VB.n_env == 4);
			CHECK(VB.n_var == 67);
			CHECK(VB.n_effects == 2);
			CHECK(VB.vp_init.muw(0) == 0.25);
			CHECK(VB.p.init_weights_with_snpwise_scan == false);
			CHECK(VB.dXtEEX(0, 0) == Approx(38.9610805993));
			CHECK(VB.dXtEEX(1, 0) == Approx(38.2995451744));

			CHECK(VB.dXtEEX(0, 4) == Approx(-2.6239467101));
			CHECK(VB.dXtEEX(1, 4) == Approx(-13.0001255314));
		}

		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
		SECTION("Ex4. Explicitly checking hyps") {
			// Set up for RunInnerLoop
			long n_grid = VB.hyps_grid.rows();
			std::vector<Hyps> all_hyps;
			VB.unpack_hyps(VB.hyps_grid, all_hyps);

			// Set up for updateAllParams
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double >> logw_updates(n_grid);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			EigenDataVector check_ym;
			Eigen::VectorXd Eq_beta;

			VB.updateAllParams(0, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(vp.alpha_beta(0)            == Approx(0.1331830674));
			CHECK(vp.alpha_beta(1)            == Approx(0.1395213065));
			CHECK(vp.alpha_beta(63)           == Approx(0.1457841418));
			CHECK(vp.muw(0, 0)              == Approx(0.1151626822));

			CHECK(hyps.sigma                == Approx(0.7035358966));
			CHECK(hyps.lambda[0]            == Approx(0.1666006426));
			CHECK(hyps.lambda[1]            == Approx(0.1350873122));
			CHECK(hyps.slab_relative_var[0] == Approx(0.0078059267));
			CHECK(hyps.slab_relative_var[1] == Approx(0.0050623453));

			Eq_beta = vp.alpha_beta * vp.mu1_beta;
			if(p.mode_mog_prior_beta) Eq_beta.array() += (1 - vp.alpha_beta) * vp.mu2_beta;
			check_ym  = VB.X * Eq_beta;
			check_ym += VB.C * vp.muc.cast<scalarData>().matrix();
			CHECK(vp.ym(0)            == Approx(check_ym(0)));

			VB.updateAllParams(1, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(vp.alpha_beta(0)            == Approx(0.1455501422));
			CHECK(vp.muw(0, 0)              == Approx(0.0675565259));
			CHECK(vp.alpha_gam(63)           == Approx(0.1181212684));
			CHECK(vp.mu1_gam(63)              == Approx(0.0019344274));
			CHECK(vp.s1_gam_sq(63)            == Approx(0.0026155945));

			CHECK(hyps.sigma                == Approx(0.6078333334));
			CHECK(hyps.lambda[0]            == Approx(0.1951731005));
			CHECK(hyps.lambda[1]            == Approx(0.1175616803));
			CHECK(hyps.slab_relative_var[0] == Approx(0.0120434663));
			CHECK(hyps.slab_relative_var[1] == Approx(0.0042684077));
			CHECK(hyps.s_x[0]               == Approx(67.0));
			CHECK(hyps.s_x[1]               == Approx(0.3089901675));
			CHECK(hyps.pve[1]               == Approx(0.0001339388));
			CHECK(hyps.pve_large[1]         == Approx(0.0001339374));

			Eq_beta = vp.alpha_beta * vp.mu1_beta;
			if(p.mode_mog_prior_beta) Eq_beta.array() += (1 - vp.alpha_beta) * vp.mu2_beta;
			check_ym  = VB.X * Eq_beta;
			check_ym += VB.C * vp.muc.cast<scalarData>().matrix();
			CHECK(vp.ym(0)            == Approx(check_ym(0)));

			VB.updateAllParams(2, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(vp.alpha_beta(63)           == Approx(0.2849674292));
			CHECK(vp.muw(0, 0)              == Approx(0.0385493631));
			CHECK(vp.alpha_gam(63)           == Approx(0.1035741368));
			CHECK(vp.mu1_gam(63)              == Approx(-0.0008068742));
			CHECK(vp.s1_gam_sq(63)            == Approx(0.0019506006));

			CHECK(hyps.sigma                == Approx(0.5604876755));
			CHECK(hyps.lambda[0]            == Approx(0.2187750882));
			CHECK(hyps.lambda[1]            == Approx(0.1024356641));
			CHECK(hyps.slab_relative_var[0] == Approx(0.0158099281));
			CHECK(hyps.slab_relative_var[1] == Approx(0.0033998137));
			CHECK(hyps.s_x[0]               == Approx(64.0));
			CHECK(hyps.s_x[1]               == Approx(0.1012145499));
			CHECK(hyps.pve[1]               == Approx(0.0000288603));
			CHECK(hyps.pve_large[1]         == Approx(0.0000288598));

			CHECK(VB.calc_logw(hyps, vp) == Approx(-88.4935443832));
			VbTracker tracker(p);
			tracker.init_interim_output(0,2, VB.n_effects, VB.n_env, VB.env_names, vp);
			tracker.dump_state(2, VB.n_covar, VB.n_var, VB.n_env, VB.n_effects, vp, hyps, VB.X, VB.covar_names, VB.env_names);

			// Checking logw
			double int_linear = -1.0 * VB.calcExpLinear(hyps, vp) / 2.0 / hyps.sigma;
			int_linear -= VB.N * std::log(2.0 * VB.PI * hyps.sigma) / 2.0;
			CHECK(int_linear  == Approx(-58.682272353));

			CHECK(VB.calcExpLinear(hyps, vp)  == Approx(30.5204787945));
			CHECK(VB.calcKLBeta(hyps, vp)  == Approx(-5.3386584503));
			CHECK(VB.calcKLGamma(hyps, vp)  == Approx(-0.0059122266));

			// check int_linear

			// Expectation of linear regression log-likelihood
			int_linear  = (VB.Y - vp.ym).squaredNorm();
			int_linear -= 2.0 * (VB.Y - vp.ym).cwiseProduct(vp.eta).dot(vp.yx);
			int_linear += vp.yx.cwiseProduct(vp.eta_sq).dot(vp.yx);
			CHECK(int_linear == Approx(21.7708079744));

			double int_linear2  = (VB.Y - vp.ym - vp.yx.cwiseProduct(vp.eta)).squaredNorm();
			int_linear2 -= vp.yx.cwiseProduct(vp.eta).squaredNorm();
			int_linear2 += vp.yx.cwiseProduct(vp.eta_sq).dot(vp.yx);
			CHECK(int_linear2 == Approx(21.7708079744));

			double kl_covar = 0.0;
			kl_covar += (double) VB.n_covar * (1.0 - std::log(hyps.sigma * VB.sigma_c)) / 2.0;
			kl_covar += vp.sc_sq.log().sum() / 2.0;
			kl_covar -= vp.sc_sq.sum() / 2.0 / hyps.sigma / VB.sigma_c;
			kl_covar -= vp.muc.square().sum() / 2.0 / hyps.sigma / VB.sigma_c;
			CHECK(kl_covar == Approx(-24.0588694492));

			// weights
			double kl_weights = 0.0;
			kl_weights += (double) VB.n_env / 2.0;
			kl_weights += vp.sw_sq.log().sum() / 2.0;
			kl_weights -= vp.sw_sq.sum() / 2.0;
			kl_weights -= vp.muw.square().sum() / 2.0;
			CHECK(kl_weights == Approx(-0.407831904));



			// variances
			CHECK(vp.sc_sq.sum() == Approx(0.0496189464));
			CHECK(vp.var_beta().sum() == Approx(0.1037151527));
			CHECK(vp.var_gam().sum() == Approx(0.0134028501));
			CHECK(vp.mean_beta().sum() == Approx(0.1211422468));
			CHECK(vp.mean_gam().sum() == Approx(0.0024171785));
			CHECK(vp.muw.sum() == Approx(0.0258090141));
			CHECK(vp.sw_sq.sum() == Approx(1.9661205603));
			CHECK((vp.EdZtZ * vp.var_gam()).sum() == Approx(1.2362999683));

			CHECK(vp.EdZtZ.sum() == Approx(6231.0737696025));
			CHECK(vp.eta_sq.sum() == Approx(96.4428364816));
			CHECK(vp.eta.squaredNorm() == Approx(0.1029290247));
			CHECK(vp.ym.squaredNorm() == Approx(14.3819142131));
			CHECK(vp.yx.squaredNorm() == Approx(0.0006402607));

			double dztz0 = (VB.X.col(0).array().square() * vp.eta_sq.array()).sum();
			CHECK(dztz0 == Approx(92.2298410065));
			CHECK(vp.EdZtZ(0) == Approx(92.2298410065));
		}

		VB.run_inference(VB.hyps_grid, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 10);
			CHECK(trackers[0].logw == Approx(-86.8089205664));
		}
	}
}

TEST_CASE( "Example 6: single-env w MoG + hyps max" ){
	parameters p;

	SECTION("Ex6. No filters applied, high mem mode"){
		char* argv[] = { (char*) "bin/bgen_prog", (char*) "--mode_vb", (char*) "--effects_prior_mog",
						 (char*) "--vb_iter_max", (char*) "20",  (char*) "--mode_regress_out_covars",
						 (char*) "--mode_empirical_bayes", (char*) "--high_mem",
						 (char*) "--bgen", (char*) "data/io_test/n50_p100.bgen",
						 (char*) "--out", (char*) "data/io_test/fake_age.out",
						 (char*) "--pheno", (char*) "data/io_test/pheno.txt",
						 (char*) "--hyps_grid", (char*) "data/io_test/hyperpriors_gxage.txt",
						 (char*) "--hyps_probs", (char*) "data/io_test/hyperpriors_gxage_probs.txt",
						 (char*) "--vb_init", (char*) "data/io_test/answer_init.txt",
						 (char*) "--environment", (char*) "data/io_test/age.txt",
						 (char*) "--spike_diff_factor", (char*) "100"};
		int argc = sizeof(argv)/sizeof(argv[0]);
		parse_arguments(p, argc, argv);
		Data data( p );

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		SECTION( "Ex6. Non genetic data standardised + covars regressed"){
			CHECK(data.params.scale_pheno == true);
			CHECK(data.params.use_vb_on_covars == false);
			CHECK(data.params.covar_file == "NULL");
//			CHECK(data.Y(0,0) == Approx(-3.6676363273605137)); Centered
//			CHECK(data.Y(0,0) == Approx(-1.5800573524786081)); Scaled
			CHECK(data.Y(0,0) == Approx(-1.262491384814441));
			CHECK(data.Y2(0,0) == Approx(-1.262491384814441));
			//CHECK(data.W(0,0) == Approx(-0.58947939694779772));
			CHECK(data.E(0,0) == Approx(-0.58947939694779772));
		}

		data.read_full_bgen();
		SECTION( "Ex6. bgen read in & standardised correctly"){
			CHECK(data.G.low_mem == false);
			CHECK(data.params.low_mem == false);
			CHECK(data.params.flip_high_maf_variants == true);
			CHECK(data.G(0, 0) == Approx(1.8604233373));
		}

		SECTION( "Ex6. Confirm calc_dxteex() reorders properly"){
			data.params.dxteex_file = "data/io_test/inputs/dxteex_mixed.txt";
			data.read_external_dxteex();
			data.calc_dxteex();
			CHECK(data.dXtEEX(0, 0) == Approx(87.204591182113916));
			CHECK(data.n_dxteex_computed == 1);
		}

		data.calc_dxteex();
		if(p.vb_init_file != "NULL"){
			data.read_alpha_mu();
		}
		VBayesX2 VB(data);
		VB.check_inputs();
		SECTION("Ex6. Vbayes_X2 initialised correctly"){
			CHECK(VB.n_samples == 50);
			CHECK(VB.N == 50.0);
			CHECK(VB.n_env == 1);
			//CHECK(VB.n_covar == 1);
			CHECK(VB.n_effects == 2);
			CHECK(VB.vp_init.muw(0) == 1.0);
			CHECK(VB.p.init_weights_with_snpwise_scan == false);
			CHECK(VB.dXtEEX(0, 0) == Approx(87.204591182113916));
		}

		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
		SECTION("Ex6. Explicitly checking updates"){
			// Set up for RunInnerLoop
			long n_grid = VB.hyps_grid.rows();
			std::vector<Hyps> all_hyps;
			VB.unpack_hyps(VB.hyps_grid, all_hyps);

			// Set up for updateAllParams
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double >> logw_updates(n_grid);

			VB.updateAllParams(0, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			CHECK(vp.alpha_beta(0) == Approx(0.1447525646));
			CHECK(vp.mu1_beta(0) == Approx(-0.0304566021));
			CHECK(vp.mu2_beta(0) == Approx(-0.0003586526));
			CHECK(vp.alpha_beta(1) == Approx(0.1515936892));
			CHECK(vp.mu1_beta(1) == Approx(-0.0356183259));
			CHECK(vp.mu2_beta(1) == Approx(-0.0004194363));
			CHECK(vp.alpha_beta(63) == Approx(0.1762251019));
			CHECK(hyps.sigma == Approx(0.3994029731));
			CHECK(hyps.lambda(0) == Approx(0.1693099847));
			CHECK(hyps.slab_var(0) == Approx(0.0056085838));
			CHECK(hyps.spike_var(0) == Approx(0.0000368515));
			CHECK(VB.calc_logw(hyps, vp) == Approx(-52.129381445));

			VB.updateAllParams(1, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			CHECK(vp.alpha_beta(0) == Approx(0.1428104733));
			CHECK(vp.mu1_beta(0) == Approx(-0.01972825));
			CHECK(vp.mu2_beta(0) == Approx(-0.0002178332));
			CHECK(vp.alpha_beta(1) == Approx(0.1580997887));
			CHECK(vp.alpha_beta(63) == Approx(0.6342565543));
			CHECK(hyps.sigma == Approx(0.2888497603));
			CHECK(hyps.lambda(0) == Approx(0.2065007836));
			CHECK(hyps.slab_var(0) == Approx(0.0077922078));
			CHECK(hyps.spike_var(0) == Approx(0.0000369985));
			CHECK(VB.calc_logw(hyps, vp) == Approx(-48.0705874648));
		}

		SECTION("Ex6. Checking rescan") {
			// Set up for RunInnerLoop
			// Set up for RunInnerLoop
			int round_index = 2;
			long n_samples = VB.n_samples;
			long n_grid = VB.hyps_grid.rows();
			std::vector<Hyps> all_hyps;
			VB.unpack_hyps(VB.hyps_grid, all_hyps);

			// Allocate trackers
			std::vector< VbTracker > trackers(n_grid, p);

			VB.runInnerLoop(false, round_index, all_hyps, trackers);


			CHECK(trackers[1].logw == Approx(-45.7823937859));
			CHECK(trackers[1].vp.eta[0] == Approx(-0.5894793969));
			CHECK(trackers[1].vp.ym[0] == Approx(-0.8185317198));

			Eigen::VectorXd gam_neglogp(VB.n_var);
			VB.rescanGWAS(trackers[1].vp, gam_neglogp);

			// Eigen::VectorXd pheno = VB.Y - trackers[1].vp.ym;
			// Eigen::VectorXd Z_kk(n_samples);
			// int jj = 1;
			// Z_kk = VB.X.col(jj).cwiseProduct(trackers[1].vp.eta);
			// double ztz_inv = 1.0 / Z_kk.dot(Z_kk);
			// double gam = Z_kk.dot(pheno) * ztz_inv;
			// double rss_null = (pheno - Z_kk * gam).squaredNorm();
			//
			// // T-test of variant j
			// boost_m::students_t t_dist(n_samples - 1);
			// double main_se_j    = std::sqrt(rss_null / (VB.N - 1.0) * ztz_inv);
			// double main_tstat_j = gam / main_se_j;
			// double main_pval_j  = 2 * boost_m::cdf(boost_m::complement(t_dist, fabs(main_tstat_j)));
			//
			// double neglogp_j = -1 * std::log10(main_pval_j);

			CHECK(gam_neglogp[1] == Approx(0.2392402716));
			// CHECK(pheno[0] == Approx(-0.4439596651));
			// CHECK(VB.X.col(jj)[0] == Approx(0.7465835328));
			// CHECK(Z_kk[0] == Approx(-0.44009531));
			// CHECK(gam == Approx(0.0223947128));
			// CHECK(main_pval_j == Approx(0.576447458));
			// CHECK(main_tstat_j == Approx(0.5623409325));
			// CHECK(main_se_j == Approx(0.0398240845));
			// CHECK(rss_null == Approx(7.9181184549));
		}

		VB.run_inference(VB.hyps_grid, false, 2, trackers);
		SECTION("Ex6. Vbayes_X2 inference correct"){
//			CHECK(trackers[0].count == 741);
//			CHECK(trackers[3].count == 71);
//			CHECK(trackers[0].logw == Approx(-45.2036994175));
//			CHECK(trackers[1].logw == Approx(-40.8450319874));
//			CHECK(trackers[2].logw == Approx(-40.960377414));
//			CHECK(trackers[3].logw == Approx(-40.9917439828));
			CHECK(trackers[0].count == 20);
			CHECK(trackers[3].count == 20);
			CHECK(trackers[0].logw == Approx(-45.8542053615));
			CHECK(trackers[1].logw == Approx(-45.7823937859));
			CHECK(trackers[2].logw == Approx(-41.3150655897));
			CHECK(trackers[3].logw == Approx(-41.639981773));
		}
	}
}

TEST_CASE("--dxteex") {
	parameters p;
	char *argv[] = {(char *) "bin/bgen_prog", (char *) "--mode_vb", (char *) "--low_mem",
					(char *) "--use_vb_on_covars", (char *) "--mode_empirical_bayes",
					(char *) "--effects_prior_mog",
					(char *) "--vb_iter_max", (char *) "10",
					(char *) "--environment", (char *) "data/io_test/n50_p100_env.txt",
					(char *) "--bgen", (char *) "data/io_test/n50_p100.bgen",
					(char *) "--out", (char *) "data/io_test/config4.out",
					(char *) "--pheno", (char *) "data/io_test/pheno.txt",
					(char *) "--hyps_grid", (char *) "data/io_test/single_hyps_gxage.txt",
					(char *) "--vb_init", (char *) "data/io_test/answer_init.txt"};
	int argc = sizeof(argv) / sizeof(argv[0]);
	parse_arguments(p, argc, argv);

	SECTION("Compute dxteex internally"){
		Data data(p);

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		SECTION("Ex4. Non genetic data standardised + covars regressed") {
			CHECK(data.E(0, 0) == Approx(0.8957059881));
		}
		data.read_full_bgen();

		data.calc_dxteex();
		data.calc_snpstats();
		if (p.vb_init_file != "NULL") {
			data.read_alpha_mu();
		}
		VBayesX2 VB(data);
		VB.check_inputs();
		SECTION("Ex4. Vbayes_X2 initialised correctly") {
			CHECK(VB.n_samples == 50);
			CHECK(VB.N == 50.0);
			CHECK(VB.n_env == 4);
			CHECK(VB.n_effects == 2);
			CHECK(VB.vp_init.muw(0) == 0.25);
			CHECK(VB.p.init_weights_with_snpwise_scan == false);
			CHECK(VB.dXtEEX(0, 0) == Approx(38.9610805993));
			CHECK(VB.dXtEEX(1, 0) == Approx(38.2995451744));
			CHECK(VB.dXtEEX(2, 0) == Approx(33.7077899144));
			CHECK(VB.dXtEEX(3, 0) == Approx(35.7391671158));

			CHECK(VB.dXtEEX(0, 4) == Approx(-2.6239467101));
			CHECK(VB.dXtEEX(1, 4) == Approx(-13.0001255314));
			CHECK(VB.dXtEEX(2, 4) == Approx(-11.6635557299));
			CHECK(VB.dXtEEX(3, 4) == Approx(-7.2154836264));
		}

		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
		VB.run_inference(VB.hyps_grid, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 10);
			CHECK(trackers[0].logw == Approx(-86.8089205664));
		}
	}

	SECTION("Compute dxteex external") {
		p.dxteex_file = "data/io_test/n50_p100_dxteex.txt";
		Data data(p);

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		SECTION("Ex4. Non genetic data standardised + covars regressed") {
			CHECK(data.E(0, 0) == Approx(0.8957059881));
		}
		data.read_full_bgen();

		data.read_external_dxteex();
		data.calc_dxteex();
		data.calc_snpstats();
		if (p.vb_init_file != "NULL") {
			data.read_alpha_mu();
		}
		VBayesX2 VB(data);
		VB.check_inputs();
		SECTION("Ex4. Vbayes_X2 initialised correctly") {
			CHECK(VB.n_samples == 50);
			CHECK(VB.N == 50.0);
			CHECK(VB.n_env == 4);
			CHECK(VB.n_effects == 2);
			CHECK(VB.vp_init.muw(0) == 0.25);
			CHECK(VB.p.init_weights_with_snpwise_scan == false);
			CHECK(VB.dXtEEX(0, 0) == Approx(38.9610805993));
			CHECK(VB.dXtEEX(1, 0) == Approx(38.3718));
			CHECK(VB.dXtEEX(2, 0) == Approx(33.81659));
			CHECK(VB.dXtEEX(3, 0) == Approx(35.8492));

			CHECK(VB.dXtEEX(0, 4) == Approx(-2.6239467101));
			CHECK(VB.dXtEEX(1, 4) == Approx(-12.96763));
			CHECK(VB.dXtEEX(2, 4) == Approx(-11.66501));
			CHECK(VB.dXtEEX(3, 4) == Approx(-7.20105));
		}

		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
		VB.run_inference(VB.hyps_grid, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 10);
			CHECK(trackers[0].logw == Approx(-86.8089149565));
		}
	}
}



TEST_CASE("--dxteex case8") {
	parameters p;
	char *argv[] = {(char *) "bin/bgen_prog", (char *) "--mode_vb", (char *) "--low_mem",
					(char *) "--mode_empirical_bayes",
					(char *) "--effects_prior_mog",
					(char *) "--use_vb_on_covars",
					(char *) "--vb_iter_max", (char *) "30",
					(char *) "--environment", (char *) "data/io_test/case8/env.txt",
					(char *) "--bgen", (char *) "data/io_test/n1000_p2000.bgen",
					(char *) "--covar", (char *) "data/io_test/case8/age.txt",
					(char *) "--out", (char *) "data/io_test/case8/inference.out",
					(char *) "--pheno", (char *) "data/io_test/case8/pheno.txt",
					(char *) "--hyps_grid", (char *) "data/io_test/case8/hyperpriors_gxage_v1.txt",
					(char *) "--vb_init", (char *) "data/io_test/case8/joint_init2.txt"};
	int argc = sizeof(argv) / sizeof(argv[0]);
	parse_arguments(p, argc, argv);

	SECTION("Compute dxteex internally"){
		Data data(p);

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		SECTION("Ex4. Non genetic data standardised + covars regressed") {
			CHECK(data.E(0, 0) == Approx(0));
		}
		data.read_full_bgen();

		data.calc_dxteex();
		data.calc_snpstats();
		if (p.vb_init_file != "NULL") {
			data.read_alpha_mu();
		}
		SECTION("Ex4. Vbayes_X2 initialised correctly") {
			CHECK(data.dXtEEX(0, 0) == Approx(0));
			CHECK(data.dXtEEX(1, 0) == Approx(0));
			CHECK(data.dXtEEX(2, 0) == Approx(0));
			CHECK(data.dXtEEX(3, 0) == Approx(0));

			CHECK(data.dXtEEX(0, 7) == Approx(-77.6736297077));
			CHECK(data.dXtEEX(1, 7) == Approx(-65.7610340352));
			CHECK(data.dXtEEX(2, 7) == Approx(-106.8630307306));
			CHECK(data.dXtEEX(3, 7) == Approx(-61.8754581783));
		}

		data.calc_snpstats();
		if (p.vb_init_file != "NULL") {
			data.read_alpha_mu();
		}

		VBayesX2 VB(data);
		VB.check_inputs();
		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
		VB.run_inference(VB.hyps_grid, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 30);
			CHECK(trackers[0].logw == Approx(-1158.9633597738));
		}
	}

	SECTION("Compute dxteex external") {
		p.dxteex_file = "data/io_test/case8/dxteex_low_mem.txt";
		Data data(p);

		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		SECTION("Ex4. Non genetic data standardised + covars regressed") {
			CHECK(data.E(0, 0) == Approx(0));
		}
		data.read_full_bgen();

		data.read_external_dxteex();
		data.calc_dxteex();
		SECTION("Ex4. Vbayes_X2 initialised correctly") {
			CHECK(data.dXtEEX(0, 0) == Approx(0));
			CHECK(data.dXtEEX(1, 0) == Approx(0));
			CHECK(data.dXtEEX(2, 0) == Approx(0));
			CHECK(data.dXtEEX(3, 0) == Approx(0));

			CHECK(data.dXtEEX(0, 7) == Approx(-77.6736297077));
			CHECK(data.dXtEEX(1, 7) == Approx(-65.5542323344));
			CHECK(data.dXtEEX(2, 7) == Approx(-106.8630307306));
			CHECK(data.dXtEEX(3, 7) == Approx(-61.8862174056));
		}

		data.calc_snpstats();
		if (p.vb_init_file != "NULL") {
			data.read_alpha_mu();
		}
		VBayesX2 VB(data);
		VB.check_inputs();
		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
		SECTION("Dump params state") {
			// Set up for RunInnerLoop
			long n_grid = VB.hyps_grid.rows();
			std::vector<Hyps> all_hyps;
			VB.unpack_hyps(VB.hyps_grid, all_hyps);

			// Set up for updateAllParams
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector<double >> logw_updates(n_grid);
			VariationalParameters &vp = all_vp[0];
			Hyps &hyps = all_hyps[0];

			VB.updateAllParams(0, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);
			VB.updateAllParams(1, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);
			VB.updateAllParams(2, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates);

			VbTracker tracker(p);
			tracker.init_interim_output(0, 2, VB.n_effects, VB.n_env, VB.env_names, vp);
			tracker.dump_state(2, VB.n_covar, VB.n_var, VB.n_env, VB.n_effects, vp, hyps, VB.X, VB.covar_names,
							   VB.env_names);
		}

		VB.run_inference(VB.hyps_grid, false, 2, trackers);
		SECTION("Ex3. Vbayes_X2 inference correct"){
			CHECK(trackers[0].count == 30);
			CHECK(trackers[0].logw == Approx(-1158.9630661443));
		}
	}
}






TEST_CASE( "Edge case 1: error in alpha" ){
	parameters p;

	SECTION("Ex1. No filters applied, low mem mode"){
		char* argv[] = { (char*) "bin/bgen_prog", (char*) "--mode_vb", (char*) "--low_mem",
						 (char*) "--mode_spike_slab",
						 (char*) "--bgen", (char*) "data/io_test/n50_p100.bgen",
						 (char*) "--out", (char*) "data/io_test/fake_age.out",
						 (char*) "--pheno", (char*) "data/io_test/pheno.txt",
						 (char*) "--hyps_grid", (char*) "data/io_test/hyperpriors_gxage.txt",
						 (char*) "--hyps_probs", (char*) "data/io_test/hyperpriors_gxage_probs.txt",
						 (char*) "--vb_init", (char*) "data/io_test/answer_init.txt",
						 (char*) "--environment", (char*) "data/io_test/age.txt"};
		int argc = sizeof(argv)/sizeof(argv[0]);
		parse_arguments(p, argc, argv);
		Data data( p );
		data.read_non_genetic_data();
		data.standardise_non_genetic_data();
		data.read_full_bgen();

		data.calc_dxteex();
		if(p.vb_init_file != "NULL"){
			data.read_alpha_mu();
		}
		VBayesX2 VB(data);
		VB.check_inputs();

		std::vector< VbTracker > trackers(VB.hyps_grid.rows(), p);
		SECTION("Ex1. Explicitly checking updates"){
			// Initialisation
#ifdef DATA_AS_FLOAT
			CHECK( (double)  VB.vp_init.ym(0) == Approx(0.0003200434));
#else
			CHECK(VB.vp_init.ym(0) == Approx(0.0003200476));
#endif
			CHECK(VB.vp_init.yx(0) == Approx(0.0081544079));
			CHECK(VB.vp_init.eta(0) == Approx(-0.5894793969));

			// Set up for RunInnerLoop
			long n_grid = VB.hyps_grid.rows();
			long n_samples = VB.n_samples;
			std::vector<Hyps> all_hyps;
			VB.unpack_hyps(VB.hyps_grid, all_hyps);

			// Set up for updateAllParams
			std::vector<VariationalParameters> all_vp;
			VB.setup_variational_params(all_hyps, all_vp);
			VariationalParameters& vp = all_vp[0];
			Hyps& hyps = all_hyps[0];

			int round_index = 2;
			std::vector<double> logw_prev(n_grid, -std::numeric_limits<double>::max());
			std::vector<std::vector< double >> logw_updates(n_grid);

			vp.alpha_beta(0) = std::nan("1");

			CHECK_THROWS(VB.updateAllParams(0, round_index, all_vp, all_hyps, logw_prev, trackers, logw_updates));
		}
	}
}