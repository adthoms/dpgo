#include "manifold/LiftedSEVariable.h"

using namespace std;
using namespace ROPTLIB;

namespace DPGO{
	LiftedSEVariable::LiftedSEVariable(int r, int d, int n){
		StiefelVariable = new StieVariable(r,d);
		EuclideanVariable = new EucVariable(r);
		CartanVariable = new ProductElement(2, StiefelVariable, 1, EuclideanVariable, 1);
		MyVariable = new ProductElement(1, CartanVariable, n);
	}

	LiftedSEVariable::~LiftedSEVariable(){
		// Avoid memory leak
		delete StiefelVariable;
		delete EuclideanVariable;
		delete CartanVariable;
		delete MyVariable;
	}

	void LiftedSEVariable::getData(Matrix& Y){
		ProductElement* T = static_cast<ProductElement*>(MyVariable->GetElement(0));
	  	const int *sizes = T->GetElement(0)->Getsize();
	  	unsigned int r = sizes[0];
	  	unsigned int d = sizes[1];
	  	unsigned int n = MyVariable->GetNumofElement();
	  	Y = Eigen::Map<Matrix>(
	        (double *)MyVariable->ObtainReadData(), r, n*(d+1));
		}

    void LiftedSEVariable::setData(Matrix& Y){
    	ProductElement* T = static_cast<ROPTLIB::ProductElement*>(MyVariable->GetElement(0));
	    const int *sizes = T->GetElement(0)->Getsize();
	    unsigned int r = sizes[0];
	    unsigned int d = sizes[1];
	    unsigned int n = MyVariable->GetNumofElement();

	    // Copy array data from Eigen matrix to ROPTLIB variable
	    const double *matrix_data = Y.data();
	    double *prodvar_data = MyVariable->ObtainWriteEntireData();
	    memcpy(prodvar_data, matrix_data, sizeof(double) * r * (d+1) * n);
    }

}