#include "gtest/gtest.h"
#include <DPGO/PGOAgent.h>

using namespace DPGO;

TEST(testDPGO, LineGraph)
{
    unsigned int id = 1;
    unsigned int d,r;
    d = 3;
    r = 3;
    ROPTALG algorithm = ROPTALG::RTR;
    bool verbose = false;
    PGOAgentParameters options(d,r,algorithm,verbose);

    Matrix R = Matrix::Identity(d, d);
    Matrix t = Matrix::Random(d,1);

    PGOAgent agent(id, options);
    for (unsigned int i = 0; i < 4; ++i){
        RelativeSEMeasurement m(id, id, i, i+1, R, t, 1.0, 1.0);
        agent.addOdometry(m);
    }
    agent.optimize();

    ASSERT_EQ(agent.getID(), id);
    ASSERT_EQ(agent.getCluster(), id);
    ASSERT_EQ(agent.num_poses(), 5);
    ASSERT_EQ(agent.dimension(), d);
    ASSERT_EQ(agent.relaxation_rank(), r);
}