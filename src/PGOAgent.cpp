/* ----------------------------------------------------------------------------
 * Copyright 2020, Massachusetts Institute of Technology, * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Yulun Tian, et al. (see README for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

#include <DPGO/DPGO_utils.h>
#include <DPGO/PGOAgent.h>
#include <DPGO/QuadraticOptimizer.h>
#include <glog/logging.h>

#include <Eigen/CholmodSupport>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>

using std::lock_guard;
using std::unique_lock;
using std::set;
using std::thread;
using std::vector;

namespace DPGO {

PGOAgent::PGOAgent(unsigned ID, const PGOAgentParameters &params)
    : mID(ID), d(params.d), r(params.r), X(r, d, 1),
      mParams(params), mState(PGOAgentState::WAIT_FOR_DATA),
      mStatus(ID, mState, 0, 0, false, 0),
      mRobustCost(params.robustCostParams),
      mPoseGraph(std::make_shared<PoseGraph>(mID, r, d)),
      mRate(10), mInstanceNumber(0), mIterationNumber(0), mNumPosesReceived(0),
      mLogger(params.logDirectory),
      gamma(0), alpha(0), Y(X), V(X), XPrev(X) {
  if (mParams.verbose) {
    std::cout << "Initializing PGO agent..." << std::endl;
    std::cout << params << std::endl;
  }
  if (mID == 0) setLiftingMatrix(fixedStiefelVariable(d, r));
}

PGOAgent::~PGOAgent() {
  // Make sure that optimization thread is not running, before exiting
  endOptimizationLoop();
}

void PGOAgent::setX(const Matrix &Xin) {
  lock_guard<mutex> lock(mPosesMutex);
  CHECK_NE(mState, PGOAgentState::WAIT_FOR_DATA);
  CHECK_EQ(Xin.rows(), relaxation_rank());
  CHECK_EQ(Xin.cols(), (dimension() + 1) * num_poses());
  mState = PGOAgentState::INITIALIZED;
  X.setData(Xin);
  if (mParams.acceleration) {
    initializeAcceleration();
  }
  LOG_IF(INFO, mParams.verbose) << "Robot " << getID() << " resets trajectory with length " << num_poses();
}

bool PGOAgent::getX(Matrix &Mout) {
  lock_guard<mutex> lock(mPosesMutex);
  Mout = X.getData();
  return true;
}

bool PGOAgent::getSharedPose(unsigned int index, Matrix &Mout) {
  if (mState != PGOAgentState::INITIALIZED)
    return false;
  lock_guard<mutex> lock(mPosesMutex);
  if (index >= num_poses()) return false;
  Mout = X.pose(index);
  return true;
}

bool PGOAgent::getAuxSharedPose(unsigned int index, Matrix &Mout) {
  CHECK(mParams.acceleration);
  if (mState != PGOAgentState::INITIALIZED)
    return false;
  lock_guard<mutex> lock(mPosesMutex);
  if (index >= num_poses()) return false;
  Mout = Y.pose(index);
  return true;
}

bool PGOAgent::getSharedPoseDict(PoseDict &map) {
  if (mState != PGOAgentState::INITIALIZED)
    return false;
  map.clear();
  lock_guard<mutex> lock(mPosesMutex);
  for (const auto &pose_id : mPoseGraph->myPublicPoseIDs()) {
    auto robot_id = pose_id.robot_id;
    auto frame_id = pose_id.frame_id;
    CHECK_EQ(robot_id, getID());
    LiftedPose Xi(X.pose(frame_id));
    map.emplace(pose_id, Xi);
  }
  return true;
}

bool PGOAgent::getAuxSharedPoseDict(PoseDict &map) {
  CHECK(mParams.acceleration);
  if (mState != PGOAgentState::INITIALIZED)
    return false;
  map.clear();
  lock_guard<mutex> lock(mPosesMutex);
  for (const auto &pose_id : mPoseGraph->myPublicPoseIDs()) {
    auto robot_id = pose_id.robot_id;
    auto frame_id = pose_id.frame_id;
    CHECK_EQ(robot_id, getID());
    LiftedPose Yi(Y.pose(frame_id));
    map.emplace(pose_id, Yi);
  }
  return true;
}

void PGOAgent::setLiftingMatrix(const Matrix &M) {
  CHECK_EQ(M.rows(), r);
  CHECK_EQ(M.cols(), d);
  YLift.emplace(M);
}

void PGOAgent::addMeasurement(const RelativeSEMeasurement &factor) {
  CHECK_EQ(mState, PGOAgentState::WAIT_FOR_DATA);
  lock_guard<mutex> mLock(mMeasurementsMutex);
  mPoseGraph->addMeasurement(factor);
}

void PGOAgent::setMeasurements(
    const std::vector<RelativeSEMeasurement> &inputOdometry,
    const std::vector<RelativeSEMeasurement> &inputPrivateLoopClosures,
    const std::vector<RelativeSEMeasurement> &inputSharedLoopClosures) {
  CHECK(!isOptimizationRunning());
  CHECK_EQ(mState, PGOAgentState::WAIT_FOR_DATA);
  if (inputOdometry.empty()) return;
  // Set pose graph measurements
  mPoseGraph = std::make_shared<PoseGraph>(mID, r, d);
  std::vector<RelativeSEMeasurement> measurements = inputOdometry;
  measurements.insert(measurements.end(), inputPrivateLoopClosures.begin(), inputPrivateLoopClosures.end());
  measurements.insert(measurements.end(), inputSharedLoopClosures.begin(), inputSharedLoopClosures.end());
  mPoseGraph->setMeasurements(measurements);
}

void PGOAgent::initialize(const PoseArray *TInitPtr) {
  CHECK_EQ(mState, PGOAgentState::WAIT_FOR_DATA);
  CHECK(!mOptimizationThread) << "Optimization thread should not be running at initialization!";

  // Do nothing if local pose graph is empty
  if (mPoseGraph->n() == 0) {
    LOG_IF(INFO, mParams.verbose) << "Local pose graph is empty. Skip initialization.";
    return;
  }

  // Check validity of initial trajectory estimate, if provided
  bool local_init = true;
  if (TInitPtr) {
    if (TInitPtr->d() == dimension() && TInitPtr->n() == num_poses())
      local_init = false;
  }

  // Update dimension for internal iterate
  X = LiftedPoseArray(relaxation_rank(), dimension(), num_poses());

  // Initialize trajectory estimate in an arbitrary frame
  if (!local_init) {
    LOG_IF(INFO, mParams.verbose) << "Using provided trajectory initialization.";
    TLocalInit.emplace(*TInitPtr);
  } else {
    LOG_IF(INFO, mParams.verbose) << "Using internal trajectory initialization.";
    initializeLocalTrajectory();
  }

  // Waiting for initialization in the GLOBAL frame
  mState = PGOAgentState::WAIT_FOR_INITIALIZATION;

  // If this robot has ID zero or if cross-robot initialization is off
  // We can initialize iterate in the global frame
  if (mID == 0 || !mParams.multirobot_initialization) {
    initializeInGlobalFrame(Pose(d));
  }
}

Pose PGOAgent::computeNeighborTransform(const RelativeSEMeasurement &measurement, const LiftedPose &neighbor_pose) {
  CHECK(YLift);
  CHECK_EQ(neighbor_pose.r(), r);
  CHECK_EQ(neighbor_pose.d(), d);

  // Notations:
  // world1: world frame before alignment
  // world2: world frame after alignment
  // frame1 : frame associated to my public pose
  // frame2 : frame associated to neighbor's public pose
  auto dT = Pose::Identity(d);
  dT.rotation() = measurement.R;
  dT.translation() = measurement.t;

  auto T_world2_frame2 = Pose::Identity(d);
  T_world2_frame2.pose() = YLift.value().transpose() * neighbor_pose.pose();

  auto T = TLocalInit.value();
  auto T_frame1_frame2 = Pose::Identity(d);
  auto T_world1_frame1 = Pose::Identity(d);
  if (measurement.r2 == getID()) {
    // Incoming edge
    T_frame1_frame2 = dT.inverse();
    T_world1_frame1.setData(T.pose(measurement.p2));
  } else {
    // Outgoing edge
    T_frame1_frame2 = dT;
    T_world1_frame1.setData(T.pose(measurement.p1));
  }
  auto T_world2_frame1 = T_world2_frame2 * T_frame1_frame2.inverse();
  auto T_world2_world1 = T_world2_frame1 * T_world1_frame1.inverse();
  checkRotationMatrix(T_world2_world1.rotation());
  return T_world2_world1;
}

bool PGOAgent::computeRobustNeighborTransformTwoStage(unsigned int neighborID,
                                                      const PoseDict &poseDict,
                                                      Pose *T_world_robot) {
  std::vector<Matrix> RVec;
  std::vector<Vector> tVec;
  // Populate candidate alignments
  // Each alignment corresponds to a single inter-robot loop closure
  for (const auto &m : mPoseGraph->sharedLoopClosuresWithRobot(neighborID)) {
    PoseID nbr_pose_id;
    nbr_pose_id.robot_id = neighborID;
    if (m.r1 == neighborID)
      nbr_pose_id.frame_id = m.p1;
    else
      nbr_pose_id.frame_id = m.p2;
    const auto &it = poseDict.find(nbr_pose_id);
    if (it != poseDict.end()) {
      const auto T = computeNeighborTransform(m, it->second);
      RVec.emplace_back(T.rotation());
      tVec.emplace_back(T.translation());
    }
  }
  if (RVec.empty()) return false;
  int m = (int) RVec.size();
  const Vector kappa = Vector::Ones(m);
  const Vector tau = Vector::Ones(m);
  Matrix ROpt;
  Vector tOpt;
  std::vector<size_t> inlierIndices;

  // Perform robust single rotation averaging
  double maxRotationError = angular2ChordalSO3(0.5);  // approximately 30 deg
  robustSingleRotationAveraging(ROpt, inlierIndices, RVec, kappa, maxRotationError);
  int inlierSize = (int) inlierIndices.size();
  printf("Robot %u attempts initialization from neighbor %u: finds %i/%i inliers.\n",
         getID(), neighborID, inlierSize, m);

  // Return if robust rotation averaging fails to find any inlier
  if (inlierIndices.size() < mParams.robustInitMinInliers) return false;

  // Perform single translation averaging on the inlier set
  std::vector<Vector> tVecInliers;
  for (const auto index : inlierIndices) {
    tVecInliers.emplace_back(tVec[index]);
  }
  singleTranslationAveraging(tOpt, tVecInliers);

  // Return transformation as matrix
  CHECK_NOTNULL(T_world_robot);
  CHECK_EQ(T_world_robot->d(), dimension());
  T_world_robot->rotation() = ROpt;
  T_world_robot->translation() = tOpt;
  return true;
}

bool PGOAgent::computeRobustNeighborTransform(unsigned int neighborID,
                                              const PoseDict &poseDict,
                                              Pose *T_world_robot) {
  std::vector<Matrix> RVec;
  std::vector<Vector> tVec;
  // Populate candidate alignments
  // Each alignment corresponds to a single inter-robot loop closure
  for (const auto &m : mPoseGraph->sharedLoopClosuresWithRobot(neighborID)) {
    PoseID nbr_pose_id;
    nbr_pose_id.robot_id = neighborID;
    if (m.r1 == neighborID)
      nbr_pose_id.frame_id = m.p1;
    else
      nbr_pose_id.frame_id = m.p2;
    const auto &it = poseDict.find(nbr_pose_id);
    if (it != poseDict.end()) {
      const auto T = computeNeighborTransform(m, it->second);
      RVec.emplace_back(T.rotation());
      tVec.emplace_back(T.translation());
    }
  }
  if (RVec.empty()) return false;
  // Perform robust single pose averaging
  int m = (int) RVec.size();
  const Vector kappa = 1.82 * Vector::Ones(m);  // rotation stddev approximately 30 degree
  const Vector tau = 0.01 * Vector::Ones(m);  // translation stddev 10 m
  const double cbar = RobustCost::computeErrorThresholdAtQuantile(0.9, 3);
  Matrix ROpt;
  Vector tOpt;
  std::vector<size_t> inlierIndices;
  robustSinglePoseAveraging(ROpt, tOpt, inlierIndices, RVec, tVec, kappa, tau, cbar);
  int inlierSize = (int) inlierIndices.size();
  printf("Robot %u attempts initialization from neighbor %u: finds %i/%i inliers.\n",
         getID(), neighborID, inlierSize, m);

  // Return if fails to identify any inlier
  if (inlierIndices.size() < mParams.robustInitMinInliers) return false;

  // Return transformation as matrix
  CHECK_NOTNULL(T_world_robot);
  CHECK_EQ(T_world_robot->d(), dimension());
  T_world_robot->rotation() = ROpt;
  T_world_robot->translation() = tOpt;
  return true;
}

void PGOAgent::initializeInGlobalFrame(const Pose &T_world_robot) {
  CHECK(YLift);
  CHECK_EQ(T_world_robot.d(), dimension());
  checkRotationMatrix(T_world_robot.rotation());

  // Halt optimization
  bool optimizationHalted = false;
  if (isOptimizationRunning()) {
    LOG_IF(INFO, mParams.verbose) << "Robot " << getID() << "halting optimization thread...";
    optimizationHalted = true;
    endOptimizationLoop();
  }

  // Halt insertion of new poses
  lock_guard<mutex> tLock(mPosesMutex);

  // Halt insertion of new measurements
  lock_guard<mutex> mLock(mMeasurementsMutex);

  // Clear cache
  lock_guard<mutex> nLock(mNeighborPosesMutex);
  neighborPoseDict.clear();
  neighborAuxPoseDict.clear();

  // Apply global transformation to local trajectory estimate
  auto T = TLocalInit.value();
  for (size_t i = 0; i < num_poses(); ++i) {
    Pose T_robot_frame(T.pose(i));
    Pose T_world_frame = T_world_robot * T_robot_frame;
    T.pose(i) = T_world_frame.pose();
  }

  // Lift back to correct relaxation rank
  X.setData(YLift.value() * T.getData());
  XInit.emplace(X);

  // Change state for this agent
  if (mState == PGOAgentState::INITIALIZED) {
    LOG_IF(INFO, mParams.verbose) << "Robot " << getID() << " re-initializes in global frame!";
  } else {
    LOG_IF(INFO, mParams.verbose) << "Robot " << getID() << " initializes in global frame!";
    mState = PGOAgentState::INITIALIZED;
  }

  // Initialize auxiliary variables
  if (mParams.acceleration) {
    initializeAcceleration();
  }

  // Log initial trajectory
  if (mParams.logData) {
    mLogger.logTrajectory(dimension(), num_poses(), T.getData(), "trajectory_initial.csv");
  }

  if (optimizationHalted) startOptimizationLoop(mRate);
}

void PGOAgent::updateNeighborPoses(unsigned neighborID, const PoseDict &poseDict) {
  CHECK(neighborID != mID);
  if (!hasNeighborStatus(neighborID)) return;
  const auto neighborState = getNeighborStatus(neighborID).state;
  if (mState == PGOAgentState::WAIT_FOR_INITIALIZATION) {
    Pose T_world_robot(dimension());
    if (computeRobustNeighborTransformTwoStage(neighborID, poseDict, &T_world_robot)) {
      initializeInGlobalFrame(T_world_robot);
    }
  }
  // Save neighbor public poses in local cache
  lock_guard<mutex> lock(mNeighborPosesMutex);
  for (const auto &it : poseDict) {
    const auto nID = it.first;
    const auto var = it.second;
    CHECK_EQ(nID.robot_id, neighborID);
    CHECK_EQ(var.r(), r);
    CHECK_EQ(var.d(), d);
    mNumPosesReceived++;
    if (!mPoseGraph->hasNeighborPose(nID))
      continue;
    // Only save poses from neighbors if this agent is initialized
    // and if the sending agent is also initialized
    if (mState == PGOAgentState::INITIALIZED && neighborState == PGOAgentState::INITIALIZED) {
      neighborPoseDict[nID] = var;
    }
  }
}

void PGOAgent::updateAuxNeighborPoses(unsigned neighborID, const PoseDict &poseDict) {
  CHECK(mParams.acceleration);
  CHECK(neighborID != mID);
  if (!hasNeighborStatus(neighborID)) return;
  const auto neighborState = getNeighborStatus(neighborID).state;
  lock_guard<mutex> lock(mNeighborPosesMutex);
  for (const auto &it : poseDict) {
    const auto nID = it.first;
    const auto var = it.second;
    CHECK(nID.robot_id == neighborID);
    CHECK(var.r() == r);
    CHECK(var.d() == d);
    mNumPosesReceived++;
    if (!mPoseGraph->hasNeighborPose(nID))
      continue;
    // Only save poses from neighbors if this agent is initialized
    // and if the sending agent is also initialized
    if (mState == PGOAgentState::INITIALIZED && neighborState == PGOAgentState::INITIALIZED) {
      neighborAuxPoseDict[nID] = var;
    }
  }
}

bool PGOAgent::getTrajectoryInLocalFrame(Matrix &Trajectory) {
  if (mState != PGOAgentState::INITIALIZED) {
    return false;
  }
  lock_guard<mutex> lock(mPosesMutex);

  PoseArray T(d, num_poses());
  T.setData(X.rotation(0).transpose() * X.getData());
  auto t0 = T.translation(0);

  // Project each rotation block to the rotation group, and make the first translation zero
  for (unsigned i = 0; i < num_poses(); ++i) {
    T.rotation(i) = projectToRotationGroup(T.rotation(i));
    T.translation(i) = T.translation(i) - t0;
  }

  Trajectory = T.getData();
  return true;
}

bool PGOAgent::getTrajectoryInGlobalFrame(Matrix &Trajectory) {
  if (!globalAnchor) return false;
  auto Xa = globalAnchor.value();
  CHECK(Xa.r() == relaxation_rank());
  CHECK(Xa.d() == dimension());
  if (mState != PGOAgentState::INITIALIZED) return false;
  lock_guard<mutex> lock(mPosesMutex);

  PoseArray T(d, num_poses());
  T.setData(Xa.rotation().transpose() * X.getData());
  Vector t0 = Xa.rotation().transpose() * Xa.translation();

  // Project each rotation block to the rotation group, and make the first translation zero
  for (unsigned i = 0; i < num_poses(); ++i) {
    T.rotation(i) = projectToRotationGroup(T.rotation(i));
    T.translation(i) = T.translation(i) - t0;
  }

  Trajectory = T.getData();
  return true;
}

bool PGOAgent::getPoseInGlobalFrame(unsigned int poseID, Matrix &T) {
  if (!globalAnchor) return false;
  auto Xa = globalAnchor.value();
  CHECK(Xa.r() == relaxation_rank());
  CHECK(Xa.d() == dimension());
  if (mState != PGOAgentState::INITIALIZED) return false;
  lock_guard<mutex> lock(mPosesMutex);
  if (poseID < 0 || poseID >= num_poses()) return false;
  Matrix Ya = Xa.rotation();
  Matrix pa = Xa.translation();
  Matrix t0 = Ya.transpose() * pa;
  Matrix Xi = X.pose(poseID);
  Matrix Ti = Ya.transpose() * Xi;
  Ti.col(d) -= t0;
  CHECK(Ti.rows() == d);
  CHECK(Ti.cols() == d + 1);
  T = Ti;
  return true;
}

bool PGOAgent::getNeighborPoseInGlobalFrame(unsigned int neighborID, unsigned int poseID, Matrix &T) {
  if (!globalAnchor) return false;
  auto Xa = globalAnchor.value();
  CHECK(Xa.r() == relaxation_rank());
  CHECK(Xa.d() == dimension());
  if (mState != PGOAgentState::INITIALIZED) return false;
  lock_guard<mutex> lock(mNeighborPosesMutex);
  PoseID nID(neighborID, poseID);
  if (neighborPoseDict.find(nID) != neighborPoseDict.end()) {
    Matrix Ya = Xa.rotation();
    Matrix pa = Xa.translation();
    Matrix t0 = Ya.transpose() * pa;
    auto Xi = neighborPoseDict.at(nID);
    CHECK(Xi.r() == r);
    CHECK(Xi.d() == d);
    Pose Ti(Ya.transpose() * Xi.pose());
    Ti.translation() -= t0;
    T = Ti.pose();
    return true;
  }
  return false;
}

std::vector<unsigned> PGOAgent::getNeighborPublicPoses(
    const unsigned &neighborID) const {
  // Check that neighborID is indeed a neighbor of this agent
  CHECK(mPoseGraph->hasNeighbor(neighborID));
  std::vector<unsigned> poseIndices;
  for (PoseID pair : mPoseGraph->neighborPublicPoseIDs()) {
    if (pair.robot_id == neighborID) {
      poseIndices.push_back(pair.frame_id);
    }
  }
  return poseIndices;
}

std::vector<unsigned> PGOAgent::getNeighbors() const {
  auto neighborRobotIDs = mPoseGraph->neighborIDs();
  std::vector<unsigned> v(neighborRobotIDs.size());
  std::copy(neighborRobotIDs.begin(), neighborRobotIDs.end(), v.begin());
  return v;
}

void PGOAgent::reset() {
  // Terminate optimization thread if running
  endOptimizationLoop();

  if (mParams.logData) {
    // Save measurements (including final weights)
    std::vector<RelativeSEMeasurement> measurements = mPoseGraph->measurements();
    mLogger.logMeasurements(measurements, "measurements.csv");

    // Save trajectory estimates after rounding
    Matrix T;
    if (getTrajectoryInGlobalFrame(T)) {
      mLogger.logTrajectory(dimension(), num_poses(), T, "trajectory_optimized.csv");
      std::cout << "Saved optimized trajectory to " << mParams.logDirectory << std::endl;
    }

    // Save solution before rounding
    writeMatrixToFile(X.getData(), mParams.logDirectory + "X.txt");
  }

  mInstanceNumber++;
  mIterationNumber = 0;
  mNumPosesReceived = 0;

  // Assume that the old lifting matrix can still be used
  mState = PGOAgentState::WAIT_FOR_DATA;
  mStatus = PGOAgentStatus(getID(), mState, mInstanceNumber, mIterationNumber, false, 0);

  neighborPoseDict.clear();
  neighborAuxPoseDict.clear();
  mTeamStatus.clear();

  mRobustCost.reset();
  globalAnchor.reset();
  TLocalInit.reset();
  XInit.reset();

  mOptimizationRequested = false;
  mPublishPublicPosesRequested = false;
  mPublishWeightsRequested = false;

  X = LiftedPoseArray(r, d, 1);
  mPoseGraph = std::make_shared<PoseGraph>(mID, r, d);
}

void PGOAgent::iterate(bool doOptimization) {
  mIterationNumber++;
  // lock pose update
  unique_lock<mutex> tLock(mPosesMutex);
  // lock measurements
  unique_lock<mutex> mLock(mMeasurementsMutex);
  // lock neighbor pose update
  unique_lock<mutex> nLock(mNeighborPosesMutex);

  // Update measurement weights (GNC)
  if (shouldUpdateLoopClosureWeights()) {
    updateLoopClosuresWeights();
    mRobustCost.update();
    // If warm start is disabled, reset trajectory estimate to initial guess
    if (!mParams.robustOptWarmStart) {
      CHECK(XInit);
      X = XInit.value();
      printf("Warm start is disabled. Robot %u resets trajectory estimates.\n", getID());
    }
    // Reset acceleration
    if (mParams.acceleration) {
      initializeAcceleration();
    }

  }

  // Perform iteration
  if (mState == PGOAgentState::INITIALIZED) {
    // Save current iterate
    XPrev = X;

    bool success;
    if (mParams.acceleration) {
      updateGamma();
      updateAlpha();
      updateY();
      success = updateX(doOptimization, true);
      updateV();
      // Check restart condition
      if (shouldRestart()) {
        restartNesterovAcceleration(doOptimization);
      }
      mPublishPublicPosesRequested = true;
    } else {
      success = updateX(doOptimization, false);
      if (doOptimization)
        mPublishPublicPosesRequested = true;
    }

    // Update status after local optimization step
    if (doOptimization) {
      mStatus.agentID = getID();
      mStatus.state = mState;
      mStatus.instanceNumber = instance_number();
      mStatus.iterationNumber = iteration_number();
      mStatus.relativeChange = LiftedPoseArray::averageTranslationDistance(X, XPrev);
      // Check local termination condition
      bool readyToTerminate = true;
      if (!success) readyToTerminate = false;
      if (mStatus.relativeChange > mParams.relChangeTol) readyToTerminate = false;
      // Compute percentage of converged loop closures (i.e., either accepted or rejected)
      const auto stat = mPoseGraph->statistics();
      double ratio = (stat.accept_loop_closures + stat.reject_loop_closures) / stat.total_loop_closures;
      if (mParams.verbose) {
        printf("Robot %u :\n "
               "accepted loop closures: %f\n "
               "rejected loop closures: %f\n "
               "undecided loop closures: %f\n",
               mID,
               stat.accept_loop_closures,
               stat.reject_loop_closures,
               stat.total_loop_closures - stat.reject_loop_closures - stat.accept_loop_closures);
      }
      if (ratio < mParams.robustOptMinConvergenceRatio) readyToTerminate = false;
      mStatus.readyToTerminate = readyToTerminate;
    }
  }
}

void PGOAgent::startOptimizationLoop(double freq) {
  // Asynchronous updates currently restricted to non-accelerated updates
  CHECK(!mParams.acceleration);

  if (isOptimizationRunning()) {
    if (mParams.verbose)
      printf("startOptimizationLoop: optimization thread already running! \n");
    return;
  }

  mRate = freq;

  mOptimizationThread = new thread(&PGOAgent::runOptimizationLoop, this);
}

void PGOAgent::runOptimizationLoop() {
  if (mParams.verbose)
    printf("Robot %u optimization thread running at %f Hz.\n", getID(), mRate);

  // Create exponential distribution with the desired rate
  std::random_device rd;  // Will be used to obtain a seed for the random number engine
  std::mt19937 rng(rd());  // Standard mersenne_twister_engine seeded with rd()
  std::exponential_distribution<double> ExponentialDistribution(mRate);

  while (true) {
    double sleepUs =
        1e6 * ExponentialDistribution(rng);  // sleeping time in microsecond

    usleep(sleepUs);

    iterate(true);

    // Check if finish requested
    if (mEndLoopRequested) {
      break;
    }
  }
}

void PGOAgent::endOptimizationLoop() {
  if (!isOptimizationRunning()) return;

  mEndLoopRequested = true;

  // wait for thread to finish
  mOptimizationThread->join();

  delete mOptimizationThread;

  mOptimizationThread = nullptr;

  mEndLoopRequested = false;  // reset request flag

  if (mParams.verbose)
    printf("Robot %u optimization thread exited. \n", getID());
}

bool PGOAgent::isOptimizationRunning() {
  return mOptimizationThread != nullptr;
}

void PGOAgent::initializeLocalTrajectory() {
  Matrix T0;
  if (mParams.robustCostParams.costType == RobustCostParameters::Type::L2) {
    T0 = chordalInitialization(dimension(), num_poses(), mPoseGraph->localMeasurements());
  } else {
    // In robust mode, we do not trust the loop closures and hence initialize from odometry
    T0 = odometryInitialization(dimension(), num_poses(), mPoseGraph->odometry());
  }
  CHECK(T0.rows() == d);
  CHECK(T0.cols() == (d + 1) * num_poses());
  PoseArray T(d, num_poses());
  T.setData(T0);
  TLocalInit.emplace(T);
}

Matrix PGOAgent::localPoseGraphOptimization() {
  // Compute initialization if necessary
  if (!TLocalInit)
    initializeLocalTrajectory();

  // Form optimization problem
  auto local_graph = std::make_shared<PoseGraph>(mID, d, d);
  local_graph->setMeasurements(mPoseGraph->localMeasurements());
  QuadraticProblem problem(local_graph);

  // Initialize optimizer object
  QuadraticOptimizer optimizer(&problem);
  optimizer.setVerbose(mParams.verbose);
  optimizer.setTrustRegionInitialRadius(10);
  optimizer.setTrustRegionIterations(10);
  optimizer.setTrustRegionTolerance(1e-1);
  optimizer.setTrustRegionMaxInnerIterations(50);

  // Optimize
  Matrix Topt = optimizer.optimize(TLocalInit.value().getData());
  if (mParams.verbose) printf("Optimization time: %f sec.\n", optimizer.getOptResult().elapsedMs / 1e3);
  return Topt;
}

bool PGOAgent::getLiftingMatrix(Matrix &M) const {
  CHECK(mID == 0);
  if (YLift.has_value()) {
    M = YLift.value();
    return true;
  }
  return false;
}

void PGOAgent::setGlobalAnchor(const Matrix &M) {
  CHECK(M.rows() == relaxation_rank());
  CHECK(M.cols() == dimension() + 1);
  LiftedPose Xa(r, d);
  Xa.pose() = M;
  globalAnchor.emplace(Xa);
}

bool PGOAgent::shouldTerminate() {
  // terminate if reached maximum iterations
  if (iteration_number() > mParams.maxNumIters) {
    printf("Reached maximum iterations.\n");
    return true;
  }

  // terminate only if all robots meet conditions
  for (size_t robot_id = 0; robot_id < mParams.numRobots; ++robot_id) {
    const auto &it = mTeamStatus.find(robot_id);
    // ready false if robot status not available
    if (it == mTeamStatus.end())
      return false;
    const auto &robot_status = it->second;
    CHECK_EQ(robot_status.agentID, robot_id);
    // return false if this robot is not initialized
    if (robot_status.state != PGOAgentState::INITIALIZED)
      return false;
    // return false if this robot is not ready to terminate
    if (!robot_status.readyToTerminate)
      return false;
  }

  return true;
}

bool PGOAgent::shouldRestart() const {
  if (mParams.acceleration) {
    return ((mIterationNumber + 1) % mParams.restartInterval == 0);
  }
  return false;
}

void PGOAgent::restartNesterovAcceleration(bool doOptimization) {
  if (mParams.acceleration && mState == PGOAgentState::INITIALIZED) {
    if (mParams.verbose) {
      printf("Robot %u restarts Nesteorv acceleration.\n", getID());
    }
    X = XPrev;
    updateX(doOptimization, false);
    V = X;
    Y = X;
    gamma = 0;
    alpha = 0;
  }
}

void PGOAgent::initializeAcceleration() {
  CHECK(mParams.acceleration);
  if (mState == PGOAgentState::INITIALIZED) {
    XPrev = X;
    gamma = 0;
    alpha = 0;
    V = X;
    Y = X;
  }
}

void PGOAgent::updateGamma() {
  CHECK(mParams.acceleration);
  CHECK(mState == PGOAgentState::INITIALIZED);
  gamma = (1 + sqrt(1 + 4 * pow(mParams.numRobots, 2) * pow(gamma, 2))) / (2 * mParams.numRobots);
}

void PGOAgent::updateAlpha() {
  CHECK(mParams.acceleration);
  CHECK(mState == PGOAgentState::INITIALIZED);
  alpha = 1 / (gamma * mParams.numRobots);
}

void PGOAgent::updateY() {
  CHECK(mParams.acceleration);
  CHECK(mState == PGOAgentState::INITIALIZED);
  LiftedSEManifold manifold(relaxation_rank(), dimension(), num_poses());
  Matrix M = (1 - alpha) * X.getData() + alpha * V.getData();
  Y.setData(manifold.project(M));
}

void PGOAgent::updateV() {
  CHECK(mParams.acceleration);
  CHECK(mState == PGOAgentState::INITIALIZED);
  LiftedSEManifold manifold(relaxation_rank(), dimension(), num_poses());
  Matrix M = V.getData() + gamma * (X.getData() - Y.getData());
  V.setData(manifold.project(M));
}

bool PGOAgent::updateX(bool doOptimization, bool acceleration) {
  if (!doOptimization) {
    if (acceleration) {
      X = Y;
    }
    return true;
  }
  LOG_IF(INFO, mParams.verbose) << "Robot " << getID() << " optimizes at iteration " << iteration_number();
  if (acceleration) CHECK(mParams.acceleration);
  CHECK(mState == PGOAgentState::INITIALIZED);

  // Initialize pose graph for optimization
  if (acceleration) {
    mPoseGraph->setNeighborPoses(neighborAuxPoseDict);
  } else {
    mPoseGraph->setNeighborPoses(neighborPoseDict);
  }

  // Skip optimization if cannot construct data matrices for some reason
  if (!mPoseGraph->constructDataMatrices()) {
    LOG(WARNING) << "Robot " << getID() << " cannot construct data matrices... Skip optimization.";
    return false;
  }

  // Initialize optimizer
  QuadraticProblem problem(mPoseGraph);
  QuadraticOptimizer optimizer(&problem);
  optimizer.setVerbose(mParams.verbose);
  optimizer.setAlgorithm(mParams.algorithm);
  optimizer.setTrustRegionTolerance(1e-2); // Force optimizer to make progress
  optimizer.setTrustRegionIterations(1);
  optimizer.setTrustRegionMaxInnerIterations(10);
  optimizer.setTrustRegionInitialRadius(100);

  // Starting solution
  Matrix X0;
  if (acceleration) {
    X0 = Y.getData();
  } else {
    X0 = X.getData();
  }
  CHECK(X0.rows() == relaxation_rank());
  CHECK(X0.cols() == (dimension() + 1) * num_poses());

  // Optimize!
  X.setData(optimizer.optimize(X0));

  // Print optimization statistics
  const auto &result = optimizer.getOptResult();
  if (mParams.verbose) {
    printf("df: %f, gn0: %f, gn1: %f, df/gn0: %f\n",
           result.fInit - result.fOpt,
           result.gradNormInit,
           result.gradNormOpt,
           (result.fInit - result.fOpt) / result.gradNormInit);
  }

  return true;
}

bool PGOAgent::shouldUpdateLoopClosureWeights() const {
  // No need to update weight if using L2 cost
  if (mParams.robustCostParams.costType == RobustCostParameters::Type::L2) return false;

  return ((mIterationNumber + 1) % mParams.robustOptInnerIters == 0);
}

void PGOAgent::updateLoopClosuresWeights() {
  CHECK(mState == PGOAgentState::INITIALIZED);

  // Since edge weights change, we need to recompute the data matrices associated with the local optimization problem
  mPoseGraph->clearDataMatrices();

  // Update private loop closures
  for (auto &m : mPoseGraph->privateLoopClosures()) {
    if (m.isKnownInlier) continue;
    auto Y1 = X.rotation(m.p1);
    auto p1 = X.translation(m.p1);
    auto Y2 = X.rotation(m.p2);
    auto p2 = X.translation(m.p2);
    double residual = std::sqrt(computeMeasurementError(m, Y1, p1, Y2, p2));
    double weight = mRobustCost.weight(residual);
    m.weight = weight;
    if (mParams.verbose) {
      printf("Agent %u update edge: (%zu, %zu) -> (%zu, %zu), residual = %f, weight = %f \n",
             getID(), m.r1, m.p1, m.r2, m.p2, residual, weight);
    }
  }

  // Update shared loop closures
  // Agent i is only responsible for updating edge weights with agent j, where j > i
  for (auto &m : mPoseGraph->sharedLoopClosures()) {
    if (m.isKnownInlier) continue;
    Matrix Y1, Y2, p1, p2;
    if (m.r1 == getID()) {
      if (m.r2 < getID()) continue;

      Y1 = X.rotation(m.p1);
      p1 = X.translation(m.p1);
      const PoseID nbrPoseID(m.r2, m.p2);
      auto KVpair = neighborPoseDict.find(nbrPoseID);
      if (KVpair == neighborPoseDict.end()) {
        printf("Agent %u cannot update edge: (%zu, %zu) -> (%zu, %zu). \n",
               getID(), m.r1, m.p1, m.r2, m.p2);
        continue;
      }
      auto X2 = KVpair->second;
      Y2 = X2.rotation();
      p2 = X2.translation();
    } else {
      if (m.r1 < getID()) continue;

      Y2 = X.rotation(m.p2);
      p2 = X.translation(m.p2);
      const PoseID nbrPoseID(m.r1, m.p1);
      auto KVpair = neighborPoseDict.find(nbrPoseID);
      if (KVpair == neighborPoseDict.end()) {
        printf("Agent %u cannot update edge: (%zu, %zu) -> (%zu, %zu). \n",
               getID(), m.r1, m.p1, m.r2, m.p2);
        continue;
      }
      auto X1 = KVpair->second;
      Y1 = X1.rotation();
      p1 = X1.translation();
    }
    double residual = std::sqrt(computeMeasurementError(m, Y1, p1, Y2, p2));
    double weight = mRobustCost.weight(residual);
    m.weight = weight;
    if (mParams.verbose) {
      printf("Agent %u update edge: (%zu, %zu) -> (%zu, %zu), residual = %f, weight = %f \n",
             getID(), m.r1, m.p1, m.r2, m.p2, residual, weight);
    }
  }
  mPublishWeightsRequested = true;
}

}  // namespace DPGO