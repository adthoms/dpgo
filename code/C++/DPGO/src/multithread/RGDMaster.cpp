#include "multithread/RGDMaster.h"
#include <iostream>
#include <fstream>
#include <cassert>
#include <chrono>
#include <unistd.h>

using namespace std;

namespace DPGO{

	RGDMaster::RGDMaster(QuadraticProblem* p, Matrix Y0){
		problem = p;
		Y = Y0;
		d = problem->dimension();
		r = problem->relaxation_rank();
		n = problem->num_poses();
		
		M = new LiftedSEManifold(r,d,n);
		Var = new LiftedSEVariable(r,d,n);
		EGrad = new LiftedSEVector(r,d,n);
		RGrad = new LiftedSEVector(r,d,n);

		initialize();
	}

	RGDMaster::~RGDMaster(){
		delete M;
		delete Var;
		delete EGrad;
		delete RGrad;
	}

	void RGDMaster::initialize(){
		assert(problem != nullptr);

		// create mutexes
		vector<mutex> list(n);
		mUpdateMutexes.swap(list);		


		// compute adjacency list
		for(unsigned i = 0; i < n; ++i){
			vector<unsigned> empty_list;
			adjList.push_back(empty_list);
			for(unsigned j = 0; j < n; ++j){
				unsigned rowStart = (d+1) * i;
				unsigned colStart = (d+1) * j;
				if(problem->Q.block(rowStart, colStart, d+1, d+1).norm() > 0.1){
					adjList[i].push_back(j);
				}
			}
		}

	}

	void RGDMaster::solve(unsigned num_threads){

		if(num_threads == 0){
			cout << "At least one worker must be used. " << endl;
			return;
		}

		numWrites = 0;
		unsigned numPosesPerWorker = n / num_threads;
		assert(numPosesPerWorker != 0);
		if(numPosesPerWorker == 0){
			cout << "Idle workers detected. Try decrease the number of workers." << endl;
			return;
		}

		for(unsigned i = 0; i < num_threads; ++i){
			// initialize a new worker
			RGDWorker* worker = new RGDWorker(this, i);
			workers.push_back(worker);

			// compute the poses that this worker updates
			vector<unsigned> updateIndices;
			unsigned indexStart = numPosesPerWorker * i;
			unsigned indexEnd = numPosesPerWorker * (i+1) - 1;
			if(i == num_threads - 1){
				indexEnd = n-1;
			}
			for(unsigned idx = indexStart; idx <= indexEnd; ++idx){
				updateIndices.push_back(idx);
			}
			worker->setUpdateIndices(updateIndices);

			worker->setUpdateRate(10000);

			// worker->setStepsize(0.00001);

			// initialize thread that this worker runs on
			thread* worker_thread = new thread(&DPGO::RGDWorker::run, worker);
			threads.push_back(worker_thread);
		}

		auto startTime = std::chrono::high_resolution_clock::now();
		vector<float> costs;
		vector<float> gradnorms;
		vector<float> elapsedTimes;

		while(true){
			auto counter = std::chrono::high_resolution_clock::now() - startTime;
			double elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(counter).count();

			costs.push_back(computeCost());
			gradnorms.push_back(computeGradNorm());
			elapsedTimes.push_back(elapsedMs);

			if (computeGradNorm() < 0.1){
				// stop all workers
				for(unsigned i = 0; i < workers.size(); ++i){
					workers[i]->requestFinish();
				}
				break;
			}

			usleep(50000);
		}

		// pause until all workers finish
		for(unsigned i = 0; i < threads.size(); ++i){
			threads[i]->join();
		}

		// export results to file
		// string dirname ="/home/yulun/bitbucket/DPGO/code/C++/results";
		// ofstream file;
		// string filename;
		// filename = dirname + "/multithread.csv";
		// file.open(filename.c_str(), ofstream::out);
		// for(unsigned i = 0; i < costs.size(); ++i){
		// 	file << elapsedTimes[i] << ","
		// 		 << costs[i] << "," 
		// 		 << gradnorms[i] << std::endl;
		// }
		// file.close();

		cout << "Master finished. Total number of writes: " << numWrites << ". Elapsed time: " \
		<< elapsedTimes.back() / (float) 1000 << " seconds." << endl;

	}

	void RGDMaster::readComponent(unsigned i, Matrix& Yi){
		unsigned start = (d+1) * i;
		Yi = Y.block(0, start, r, d+1);
	}

    void RGDMaster::writeComponent(unsigned i, Matrix& Yi){
		unsigned start = (d+1) * i;
		Y.block(0, start, r, d+1) = Yi;
		numWrites++;
    }

    void RGDMaster::readDataMatrixBlock(unsigned i, unsigned j, Matrix& Qij){
    	unsigned rowStart = (d+1) * i;
    	unsigned colStart = (d+1) * j;

    	Qij = Matrix(problem->Q.block(rowStart, colStart, d+1, d+1));
    }

    float RGDMaster::computeCost(){
    	return (Y * problem->Q * Y.transpose()).trace();
    }

    float RGDMaster::computeGradNorm(){
    	// Mat2CartanProd(Y, *x);
    	Var->setData(Y);

    	// compute Euclidean gradient
    	Matrix G = 2 * Y * problem->Q;
    	// Mat2CartanProd(G, *euclideanGradient);
    	EGrad->setData(G);

    	// compute Riemannian gradient
    	// manifold->Projection(x, euclideanGradient, riemannianGradient);
    	M->getManifold()->Projection(Var->var(), EGrad->vec(), RGrad->vec());
    	Matrix RG;
    	// CartanProd2Mat(*riemannianGradient, RG);
    	RGrad->getData(RG);
    	return RG.norm();
    }
}