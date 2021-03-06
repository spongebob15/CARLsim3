#include "gtest/gtest.h"
#include "carlsim_tests.h"

#include <carlsim.h>

#include <math.h>	// log
#include <algorithm> // max

#if defined(WIN32) || defined(WIN64)
#include <periodic_spikegen.h>
#endif


/// **************************************************************************************************************** ///
/// CONDUCTANCE-BASED MODEL (COBA)
/// **************************************************************************************************************** ///


//! This test assures that the conductance peak occurs as specified by tau_rise and tau_decay, and that the peak is
//! equal to the specified weight value
TEST(COBA, synRiseTime) {
	::testing::FLAGS_gtest_death_test_style = "threadsafe";

	CARLsim* sim = NULL;

	float time_abs_error = 2.0; // 2 ms
	float wt_abs_error = 0.1; // error for wt

#ifdef __NO_CUDA__
	int numModes = 1;
#else
	int numModes = 2;
#endif

	for (int mode=0; mode<numModes; mode++) {
		int tdAMPA  = 5;
		int trNMDA  = 20;
		int tdNMDA  = 150; // make sure it's larger than trNMDA
		int tdGABAa = 6;
		int trGABAb = 100;
		int tdGABAb = 150; // make sure it's larger than trGABAb

		sim = new CARLsim("COBA.synRiseTime",mode?GPU_MODE:CPU_MODE,SILENT,0,42);
        Grid3D neur(1);
        Grid3D neur2(1);

		int g1=sim->createGroup("excit", neur2, EXCITATORY_NEURON);
		sim->setNeuronParameters(g1, 0.02f, 0.2f, -65.0f, 8.0f);

		int g3=sim->createGroup("inhib", neur, INHIBITORY_NEURON);
		sim->setNeuronParameters(g3, 0.1f,  0.2f, -65.0f, 2.0f);

		int g0=sim->createSpikeGeneratorGroup("inputExc", neur, EXCITATORY_NEURON);
		int g2=sim->createSpikeGeneratorGroup("inputInh", neur, INHIBITORY_NEURON);

		sim->connect(g0,g1,"one-to-one",RangeWeight(0.5f),1.0,RangeDelay(1),RadiusRF(-1.0),SYN_FIXED,0.0,1.0); // NMDA
		sim->connect(g2,g3,"one-to-one",RangeWeight(0.5f),1.0,RangeDelay(1),RadiusRF(-1.0),SYN_FIXED,0.0,1.0); // GABAb

		sim->setConductances(true, tdAMPA, trNMDA, tdNMDA, tdGABAa, trGABAb, tdGABAb);

		// run network for a second first, so that we know spike will happen at simTimeMs==1000
		PeriodicSpikeGenerator* spk1 = new PeriodicSpikeGenerator(true); // periodic spiking
		spk1->setRates(1.0f);
		PeriodicSpikeGenerator* spk2 = new PeriodicSpikeGenerator(true); // periodic spiking
		spk2->setRates(1.0f);
		sim->setSpikeGenerator(g0, spk1);
		sim->setSpikeGenerator(g2, spk2);
		sim->setupNetwork(true);
		sim->runNetwork(1,0,false,false);

		// now observe gNMDA, gGABAb after spike, and make sure that the time at which they're max matches the
		// analytical solution, and that the peak conductance is actually equal to the weight we set
		int tmaxNMDA = -1;
		double maxNMDA = -1;
		int tmaxGABAb = -1;
		double maxGABAb = -1;
		int nMsec = (std::max)(trNMDA+tdNMDA,trGABAb+tdGABAb)+10;
		for (int i=0; i<nMsec; i++) {
			sim->runNetwork(0,1);

			std::vector<float> gNMDA = sim->getConductanceNMDA(g1);
			std::vector<float> gGABAb = sim->getConductanceGABAb(g3);

			if (gNMDA[0] > maxNMDA) {
				tmaxNMDA=i;
				maxNMDA=gNMDA[0];
			}
			if (gGABAb[0] > maxGABAb) {
				tmaxGABAb=i;
				maxGABAb=gGABAb[0];
			}
		}

		double tmax = (-tdNMDA*trNMDA*log(1.0*trNMDA/tdNMDA))/(tdNMDA-trNMDA);
		EXPECT_NEAR(tmaxNMDA,tmax,time_abs_error); // t_max should be near the analytical solution
		EXPECT_NEAR(maxNMDA,0.5,0.5*wt_abs_error); // max should be equal to the weight

		tmax = (-tdGABAb*trGABAb*log(1.0*trGABAb/tdGABAb))/(tdGABAb-trGABAb);
		EXPECT_NEAR(tmaxGABAb,tmaxGABAb,time_abs_error); // t_max should be near the analytical solution
		EXPECT_NEAR(maxGABAb,0.5,0.5*wt_abs_error); // max should be equal to the weight times -1

		delete spk1;
		delete spk2;
		delete sim;
	}
}


/*!
 * \brief This test ensures that CPUmode and GPUmode produce the exact same conductance values over some time period
 *
 * A single neuron gets spike input over a certaint time period. Every timestep, conductance values are read out
 * and compared CPU vs GPU. Values have to be the same (within some error margin). All combinations of activating
 * receptors is tested (i.e., only AMPA, only NMDA, AMPA+NMDA, etc.). Synapses have non-zero rise and decay times.
 */
TEST(COBA, condSingleNeuronCPUvsGPU) {
	::testing::FLAGS_gtest_death_test_style = "threadsafe";

	CARLsim* sim = NULL;
	const int nGrps = 6;
	const int runDurationMs = 2000;
	int grps[nGrps] = {-1};
	std::string expectCond[nGrps] = {"AMPA","NMDA","AMPA+NMDA","GABAa","GABAb","GABAa+GABAb"};
	float expectCondStd[nGrps] = {0.01, 0.01, 0.01, 0.01, 0.01, 0.01};

	std::vector<float> gAMPA_CPU(runDurationMs, 0.0f);
	std::vector<float> gNMDA_CPU(runDurationMs, 0.0f);
	std::vector<float> gGABAa_CPU(runDurationMs, 0.0f);
	std::vector<float> gGABAb_CPU(runDurationMs, 0.0f);

	PeriodicSpikeGenerator *spkGen1, *spkGen2;

	// make it a single neuron
	// \TODO If post gets input from more than one pre, conductance values in GPUmode are a bit off. Need to
	// investigate that (aggregating rounding errors?)
	int nInput = 1;
	int nOutput = 1;
	float rate = 30.0f;
	bool spikeAtZero = true;

#ifdef __NO_CUDA__
	int numModes = 1;
#else
	int numModes = 2;
#endif
	int numMethods = 2; //Euler and Runge-Kutta integration methods.

	for (int hasIzh4=0; hasIzh4<=1; hasIzh4++) {
		for(int method = 0; method < numMethods; method++) {//integration method
			for (int mode=0; mode<numModes; mode++) {
				sim = new CARLsim("COBA.condCPUvsGPU",mode?GPU_MODE:CPU_MODE,SILENT,0,42);
				grps[0]=sim->createGroup("excAMPA", Grid3D(nOutput), EXCITATORY_NEURON);
				grps[1]=sim->createGroup("excNMDA", Grid3D(nOutput), EXCITATORY_NEURON);
				grps[2]=sim->createGroup("excAMPA+NMDA", Grid3D(nOutput), EXCITATORY_NEURON);
				grps[3]=sim->createGroup("inhGABAa", Grid3D(nOutput), INHIBITORY_NEURON);
				grps[4]=sim->createGroup("inhGABAb", Grid3D(nOutput), INHIBITORY_NEURON);
				grps[5]=sim->createGroup("inhGABAa+GABAb", Grid3D(nOutput), INHIBITORY_NEURON);
				int g0=sim->createSpikeGeneratorGroup("spike0", Grid3D(nInput), EXCITATORY_NEURON);
				int g1=sim->createSpikeGeneratorGroup("spike1", Grid3D(nInput), INHIBITORY_NEURON);

				if (hasIzh4) {
	                    // 4-param model
	                    sim->setNeuronParameters(grps[0], 0.02f, 0.2f, -65.0f, 8.0f); // RS
						sim->setNeuronParameters(grps[1], 0.02f, 0.2f, -65.0f, 8.0f); // RS
						sim->setNeuronParameters(grps[2], 0.02f, 0.2f, -65.0f, 8.0f); // RS
						sim->setNeuronParameters(grps[3], 0.1f,  0.2f, -65.0f, 2.0f); // FS
						sim->setNeuronParameters(grps[4], 0.1f,  0.2f, -65.0f, 2.0f); // FS
						sim->setNeuronParameters(grps[5], 0.1f,  0.2f, -65.0f, 2.0f); // FS
	            } else {
	                    // 9-param model
	                    // TODO: add 9-param call for regular-spiking
	                    sim->setNeuronParameters(grps[0], 100.0f, 0.7f, -60.0f, -40.0f, 0.03f, -2.0f, 35.0f, -50.0f, 100.0f);//RS
	                    sim->setNeuronParameters(grps[1], 100.0f, 0.7f, -60.0f, -40.0f, 0.03f, -2.0f, 35.0f, -50.0f, 100.0f);//RS
	                    sim->setNeuronParameters(grps[2], 100.0f, 0.7f, -60.0f, -40.0f, 0.03f, -2.0f, 35.0f, -50.0f, 100.0f);//RS
	                    sim->setNeuronParameters(grps[3], 20.0f, 1.0f, -55.0f, -40.0f, 0.15f, 8.0f, 25.0f, -55.0f, 200.0f);//FS
	                    sim->setNeuronParameters(grps[4], 20.0f, 1.0f, -55.0f, -40.0f, 0.15f, 8.0f, 25.0f, -55.0f, 200.0f);//FS
	                    sim->setNeuronParameters(grps[5], 20.0f, 1.0f, -55.0f, -40.0f, 0.15f, 8.0f, 25.0f, -55.0f, 200.0f);//FS
	            }

	            if(method)
	            	sim->setIntegrationMethod(FORWARD_EULER, 20);
	            else
	            	sim->setIntegrationMethod(RUNGE_KUTTA4, 10);


				// use some rise and decay
				sim->setConductances(true, 5, 20, 150, 6, 100, 150);

				sim->connect(g0,grps[0],"full",RangeWeight(0.001),1.0,RangeDelay(1),RadiusRF(-1),SYN_FIXED,1.0f,0.0f);//AMPA
				sim->connect(g0,grps[1],"full",RangeWeight(0.0005),1.0,RangeDelay(1),RadiusRF(-1),SYN_FIXED,0.0,1.0);//NMDA
				sim->connect(g0,grps[2],"full",RangeWeight(0.0005),1.0,RangeDelay(1),RadiusRF(-1),SYN_FIXED,0.5,0.5);//AMPA+NMDA
				sim->connect(g1,grps[3],"full",RangeWeight(0.001),1.0,RangeDelay(1),RadiusRF(-1),SYN_FIXED,1.0,0.0);//GABAa
				sim->connect(g1,grps[4],"full",RangeWeight(0.0005f),1.0,RangeDelay(1),RadiusRF(-1),SYN_FIXED,0.0,1.0);//GABAb
				sim->connect(g1,grps[5],"full",RangeWeight(0.0005f),1.0,RangeDelay(1),RadiusRF(-1),SYN_FIXED,0.5,0.5);//GABAa+b

				spkGen1 = new PeriodicSpikeGenerator(spikeAtZero);
				spkGen1->setRates(rate);
				spkGen2 = new PeriodicSpikeGenerator(spikeAtZero);
				spkGen2->setRates(rate);
				sim->setSpikeGenerator(g0, spkGen1);
				sim->setSpikeGenerator(g1, spkGen2);

				sim->setupNetwork(true);

				// run the network for 1ms, compare conductance values
				for (int i=0; i<runDurationMs; i++) {
					sim->runNetwork(0,1,false,true);

					for (int g=0; g<nGrps; g++) {
						// for all groups
						if (expectCond[g].find("AMPA")!=std::string::npos) {
							// AMPA is active
							std::vector<float> gAMPA  = sim->getConductanceAMPA(grps[g]);
							if (!mode) {
								// CPU mode: record conductance values
								gAMPA_CPU[i] = gAMPA[0];
							} else {
								// GPU mode: compare values
								EXPECT_NEAR(gAMPA_CPU[i], gAMPA[0], expectCondStd[g]);
							}
						} else if (expectCond[g].find("NMDA")!=std::string::npos) {
							std::vector<float> gNMDA  = sim->getConductanceNMDA(grps[g]);
							if (!mode) {
								gNMDA_CPU[i] = gNMDA[0];
							} else {
								EXPECT_NEAR(gNMDA_CPU[i], gNMDA[0], expectCondStd[g]);
							}
						} else if (expectCond[g].find("GABAa")!=std::string::npos) {
							std::vector<float> gGABAa  = sim->getConductanceGABAa(grps[g]);
							if (!mode) {
								gGABAa_CPU[i] = gGABAa[0];
							} else {
								EXPECT_NEAR(gGABAa_CPU[i], gGABAa[0], expectCondStd[g]);
							}
						} else if (expectCond[g].find("GABAb")!=std::string::npos) {
							std::vector<float> gGABAb  = sim->getConductanceGABAb(grps[g]);
							if (!mode) {
								gGABAb_CPU[i] = gGABAb[0];
							} else {
								EXPECT_NEAR(gGABAb_CPU[i], gGABAb[0], expectCondStd[g]);
							}
						}
					}
				}

				delete spkGen1;
				delete spkGen2;
				delete sim;
			}
		}
	}	
}

/*
 * \brief testing CARLsim COBA output (spike rates) CPU vs GPU
 *
 * This test makes sure that whatever COBA network is run, both CPU and GPU mode give the exact same output
 * in terms of spike times and spike rates.
 * The total simulation time, input rate, weight, and delay are chosen randomly.
 * Afterwards we make sure that CPU and GPU mode produce the same spike times and spike rates. 
 */
TEST(COBA, firingRateCPUvsGPU) {
	::testing::FLAGS_gtest_death_test_style = "threadsafe";

	SpikeMonitor *spkMonG0 = NULL, *spkMonG1 = NULL;
	PeriodicSpikeGenerator *spkGenG0 = NULL;
	std::vector<std::vector<int> > spkTimesG0CPU, spkTimesG1CPU, spkTimesG0GPU, spkTimesG1GPU;
	float spkRateG0CPU = 0.0f, spkRateG1CPU = 0.0f;

	float wt = 0.15268f;
	float inputRate = 25.0f;
	int runTimeMs = 526;

	int numMethods = 2; //Euler and Runge-Kutta integration methods.
//	fprintf(stderr,"runTime=%d, delay=%d, wt=%f, input=%f\n",runTimeMs,delay,wt,inputRate);
	for (int hasIzh4=0; hasIzh4<=1; hasIzh4++) {
		for(int method = 0; method < numMethods; method++) {//integration method
			#ifdef __NO_CUDA__
			for (int isGPUmode=0; isGPUmode<=0; isGPUmode++) {
			#else
			for (int isGPUmode=0; isGPUmode<=1; isGPUmode++) {
			#endif
				CARLsim sim("COBA.firingRateCPUvsGPU",isGPUmode?GPU_MODE:CPU_MODE,SILENT,0,42);
				int g1=sim.createGroup("output", 1, EXCITATORY_NEURON);
				if(hasIzh4){
					sim.setNeuronParameters(g1, 0.02f, 0.2f, -65.0f, 8.0f); // RS
				}
				else{
					sim.setNeuronParameters(g1, 100.0f, 0.7f, -60.0f, -40.0f, 0.03f, -2.0f, 35.0f, -50.0f, 100.0f);//RS
				}

				if(method)
	            	sim.setIntegrationMethod(FORWARD_EULER, 20);
	            else
	            	sim.setIntegrationMethod(RUNGE_KUTTA4, 10);

				int g0=sim.createSpikeGeneratorGroup("input", 1 ,EXCITATORY_NEURON);
				sim.setConductances(true); // make COBA explicit

				sim.connect(g0, g1, "full", RangeWeight(wt), 1.0f, RangeDelay(1));

				bool spikeAtZero = true;
				PeriodicSpikeGenerator spkGenG0(spikeAtZero);
				spkGenG0.setRates(inputRate);
				sim.setSpikeGenerator(g0, &spkGenG0);

				sim.setupNetwork();

				spkMonG0 = sim.setSpikeMonitor(g0,"NULL");
				spkMonG1 = sim.setSpikeMonitor(g1,"NULL");

				spkMonG0->startRecording();
				spkMonG1->startRecording();
				sim.runNetwork(runTimeMs/1000,runTimeMs%1000,false);
				spkMonG0->stopRecording();
				spkMonG1->stopRecording();

		//		fprintf(stderr,"input g0=%d, nid=%d\n",g0,sim.getGroupStartNeuronId(g0));
		//		fprintf(stderr,"excit g1=%d, nid=%d\n",g1,sim.getGroupStartNeuronId(g1));

				if (!isGPUmode) {
					// CPU mode: store spike times and spike rate for future comparison
					spkRateG0CPU = spkMonG0->getPopMeanFiringRate();
					spkRateG1CPU = spkMonG1->getPopMeanFiringRate();
					spkTimesG0CPU = spkMonG0->getSpikeVector2D();
					spkTimesG1CPU = spkMonG1->getSpikeVector2D();
				} else {
					// GPU mode: compare to CPU results

					// do not ASSERT_, otherwise CARLsim will not be correctly deallocated
					// instead, use EXPECT_ and subsequent if-else condition
					bool isRateCorrectG0 = spkMonG0->getPopMeanFiringRate() == spkRateG0CPU;
					bool isRateCorrectG1 = spkMonG1->getPopMeanFiringRate() == spkRateG1CPU;
					EXPECT_TRUE(isRateCorrectG0);
					EXPECT_TRUE(isRateCorrectG1);

					if (isRateCorrectG0 && isRateCorrectG1) {
						spkTimesG0GPU = spkMonG0->getSpikeVector2D();
						spkTimesG1GPU = spkMonG1->getSpikeVector2D();
						bool isSpkSzCorrectG0 = spkTimesG0CPU[0].size() == spkTimesG0GPU[0].size();
						bool isSpkSzCorrectG1 = spkTimesG1CPU[0].size() == spkTimesG1GPU[0].size();
						EXPECT_TRUE(isSpkSzCorrectG0);
						EXPECT_TRUE(isSpkSzCorrectG1);

						if (isSpkSzCorrectG0 && isSpkSzCorrectG1) {
							for (int i=0; i<spkTimesG0CPU[0].size(); i++)
								EXPECT_EQ(spkTimesG0CPU[0][i], spkTimesG0GPU[0][i]);
							for (int i=0; i<spkTimesG1CPU[0].size(); i++)
								EXPECT_EQ(spkTimesG1CPU[0][i], spkTimesG1GPU[0][i]);
						}
					}
				}
			}
		}
	}

}

/*
 * \brief testing CARLsim COBA output (spike rates) CPU vs GPU
 *
 * This test makes sure that whatever COBA network is run, both CPU and GPU mode give the exact same output
 * in terms of spike times and spike rates.
 * The total simulation time, input rate, weight, and delay are chosen randomly.
 * Afterwards we make sure that CPU and GPU mode produce the same spike times and spike rates. 
 */
TEST(COBA, firingRateCPUandGPU_EULERvsRUNGE_KUTTA) {
	::testing::FLAGS_gtest_death_test_style = "threadsafe";

	SpikeMonitor *spkMonG0 = NULL, *spkMonG1 = NULL;
	PeriodicSpikeGenerator *spkGenG0 = NULL;
	std::vector<std::vector<int> > spkTimesG0EULER, spkTimesG1EULER, spkTimesG0RK, spkTimesG1RK;
	float spkRateG0EULER = 0.0f, spkRateG1EULER = 0.0f;

	float wt = 0.15268f;
	float inputRate = 25.0f;
	int runTimeMs = 526;

	int numMethods = 2; //Euler and Runge-Kutta integration methods.
//	fprintf(stderr,"runTime=%d, delay=%d, wt=%f, input=%f\n",runTimeMs,delay,wt,inputRate);
	for (int hasIzh4=0; hasIzh4<=1; hasIzh4++) {
		#ifdef __NO_CUDA__
		for (int isGPUmode=0; isGPUmode<=0; isGPUmode++) {
		#else
		for (int isGPUmode=0; isGPUmode<=1; isGPUmode++) {
		#endif
			for(int method = 0; method < numMethods; method++) {//integration method
				CARLsim sim("COBA.firingRateCPUvsGPU",isGPUmode?GPU_MODE:CPU_MODE,SILENT,0,42);
				int g1=sim.createGroup("output", 1, EXCITATORY_NEURON);
				if(hasIzh4){
					sim.setNeuronParameters(g1, 0.02f, 0.2f, -65.0f, 8.0f); // RS
				}
				else{
					sim.setNeuronParameters(g1, 100.0f, 0.7f, -60.0f, -40.0f, 0.03f, -2.0f, 35.0f, -50.0f, 100.0f);//RS
				}

				if(method)
	            	sim.setIntegrationMethod(RUNGE_KUTTA4, 20);
	            else
	            	sim.setIntegrationMethod(FORWARD_EULER, 30);

				int g0=sim.createSpikeGeneratorGroup("input", 1 ,EXCITATORY_NEURON);
				sim.setConductances(true); // make COBA explicit

				sim.connect(g0, g1, "full", RangeWeight(wt), 1.0f, RangeDelay(1));

				bool spikeAtZero = true;
				PeriodicSpikeGenerator spkGenG0(spikeAtZero);
				spkGenG0.setRates(inputRate);
				sim.setSpikeGenerator(g0, &spkGenG0);

				sim.setupNetwork();

				spkMonG0 = sim.setSpikeMonitor(g0,"NULL");
				spkMonG1 = sim.setSpikeMonitor(g1,"NULL");

				spkMonG0->startRecording();
				spkMonG1->startRecording();
				sim.runNetwork(runTimeMs/1000,runTimeMs%1000,false);
				spkMonG0->stopRecording();
				spkMonG1->stopRecording();

		//		fprintf(stderr,"input g0=%d, nid=%d\n",g0,sim.getGroupStartNeuronId(g0));
		//		fprintf(stderr,"excit g1=%d, nid=%d\n",g1,sim.getGroupStartNeuronId(g1));

				if (!method) {
					// Euler method: store spike times and spike rate for future comparison
					spkRateG0EULER = spkMonG0->getPopMeanFiringRate();
					spkRateG1EULER = spkMonG1->getPopMeanFiringRate();
					spkTimesG0EULER = spkMonG0->getSpikeVector2D();
					spkTimesG1EULER = spkMonG1->getSpikeVector2D();
				} else {
					// Runge-Kutta method: compare to Euler results

					// do not ASSERT_, otherwise CARLsim will not be correctly deallocated
					// instead, use EXPECT_ and subsequent if-else condition
					bool isRateCorrectG0 = spkMonG0->getPopMeanFiringRate() == spkRateG0EULER;
					bool isRateCorrectG1 = spkMonG1->getPopMeanFiringRate() == spkRateG1EULER;
					EXPECT_TRUE(isRateCorrectG0);
					EXPECT_TRUE(isRateCorrectG1);

					if (isRateCorrectG0 && isRateCorrectG1) {
						spkTimesG0RK = spkMonG0->getSpikeVector2D();
						spkTimesG1RK = spkMonG1->getSpikeVector2D();
						bool isSpkSzCorrectG0 = spkTimesG0EULER[0].size() == spkTimesG0RK[0].size();
						bool isSpkSzCorrectG1 = spkTimesG1EULER[0].size() == spkTimesG1RK[0].size();
						EXPECT_TRUE(isSpkSzCorrectG0);
						EXPECT_TRUE(isSpkSzCorrectG1);

						if (isSpkSzCorrectG0 && isSpkSzCorrectG1) {
							for (int i=0; i<spkTimesG0EULER[0].size(); i++)
								EXPECT_EQ(spkTimesG0EULER[0][i], spkTimesG0RK[0][i]);
							for (int i=0; i<spkTimesG1EULER[0].size(); i++)
								EXPECT_EQ(spkTimesG1EULER[0][i], spkTimesG1RK[0][i]);
						}
					}
				}
			}
		}
	}

}
