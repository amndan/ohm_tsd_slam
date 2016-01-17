/*
 * ThreadLocalize.cpp
 *
 *  Created on: 28.10.2014
 *      Author: phil
 */

#include "ThreadLocalize.h"

#include "SlamNode.h"
#include "ThreadMapping.h"

#include "obcore/math/linalg/linalg.h"
#include "obcore/base/Logger.h"


#include <boost/bind.hpp>

#include <cstring>
#include <unistd.h>

#include <geometry_msgs/PoseStamped.h>
#include <tf/tf.h>

//#define TRACE

namespace ohm_tsd_slam
{

ThreadLocalize::ThreadLocalize(obvious::TsdGrid* grid, ThreadMapping* mapper, ros::NodeHandle* nh, std::string nameSpace,
    const double xOffset, const double yOffset):
                    ThreadSLAM(*grid),
                    _nh(nh),
                    _mapper(*mapper),
                    _sensor(NULL),
                    _initialized(false),
                    _gridWidth(grid->getCellsX() * grid->getCellSize()),
                    _gridHeight(grid->getCellsY() * grid->getCellSize()),
                    _gridOffSetX(-1.0 * (grid->getCellsX() * grid->getCellSize() * 0.5 + xOffset)),
                    _gridOffSetY(-1.0 * (grid->getCellsY()* grid->getCellSize() * 0.5 + yOffset)),
                    _xOffset(xOffset),
                    _yOffset(yOffset),
                    _nameSpace(nameSpace)
{
  ros::NodeHandle prvNh("~");


  /*** Read parameters from ros parameter server. Use namespace if provided ***/
  _nameSpace = nameSpace;
  std::string::iterator it = _nameSpace.end() - 1;
  if (*it != '/' && _nameSpace.size()>0)
    _nameSpace += "/";

  //pose
  std::string poseTopic;
  prvNh.param(_nameSpace + "pose_topic", poseTopic, std::string("default_ns/pose"));
  poseTopic = _nameSpace + poseTopic;

  //frames
  std::string tfBaseFrameId;
  prvNh.param("tf_base_frame", tfBaseFrameId, std::string("/map"));

  std::string tfChildFrameId;
  prvNh.param(_nameSpace + "tf_child_frame", tfChildFrameId, std::string("default_ns/laser"));

  double distFilterMax = 0.0;
  double distFilterMin = 0.0;
  int icpIterations = 0;

  //ICP Options
  prvNh.param<double>(_nameSpace + "dist_filter_min", distFilterMin, DIST_FILT_MIN);
  prvNh.param<double>(_nameSpace + "dist_filter_max", distFilterMax, DIST_FILT_MAX);
  prvNh.param<int>(_nameSpace + "icp_iterations", icpIterations, ICP_ITERATIONS);

  //Maximum allowed offset between to aligned scans
  prvNh.param<double>("reg_trs_max", _trnsMax, TRNS_THRESH);
  prvNh.param<double>("reg_sin_rot_max", _rotMax, ROT_THRESH);

  //RandomMatcher options
  int trials;
  double epsThresh;
  int sizeControlSet;
  double zhit;
  double zphi;
  double zshort;
  double zmax;
  double zrand;
  double percentagePointsInC;
  double rangemax;
  double sigphi;
  double sighit;
  double lamshort;
  double maxAngleDiff;
  double maxAnglePenalty;

  prvNh.param<int>("trials", trials, 100);
  prvNh.param<double>("epsThresh", epsThresh, 0.15);
  prvNh.param<int>("sizeControlSet", sizeControlSet, 140);
  prvNh.param<double>("zhit", zhit, 0.45);
  prvNh.param<double>("zphi", zphi, 0);
  prvNh.param<double>("zshort", zshort, 0.25);
  prvNh.param<double>("zmax", zmax, 0.05);
  prvNh.param<double>("zrand", zrand, 0.25);
  prvNh.param<double>("percentagePointsInC", percentagePointsInC, 0.9);
  prvNh.param<double>("rangemax", rangemax, 20);
  prvNh.param<double>("sigphi", sigphi, M_PI / 180.0 * 3);
  prvNh.param<double>("sighit", sighit, 0.2);
  prvNh.param<double>("lamshort", lamshort, 0.08);
  prvNh.param<double>("maxAngleDiff", maxAngleDiff, 3.0);
  prvNh.param<double>("maxAnglePenalty", maxAnglePenalty, 0.5);


  //ransac options
  int paramInt = 0;
  prvNh.param<int>(nameSpace + "ransac_trials", paramInt, RANSAC_TRIALS);
  _ranTrials = static_cast<unsigned int>(paramInt);
  prvNh.param<double>(nameSpace + "ransac_eps_thresh", _ranEpsThresh, RANSAC_EPS_THRESH);
  prvNh.param<int>(nameSpace + "ransac_ctrlset_size", paramInt, RANSAC_CTRL_SET_SIZE);
  _ranSizeCtrlSet = static_cast<unsigned int>(paramInt);
  prvNh.param<double>(_nameSpace + "ransac_phi_max", _ranPhiMax, 30.0);

  int iVar = 0;
  prvNh.param<int>(_nameSpace + "registration_mode", iVar, ICP);
  _regMode = static_cast<EnumRegModes>(iVar);

  /** Align Laser scans */
  switch(_regMode)
  {
  case ICP:
    // dont need any instance
    break;
  case EXP:
    _RandomNormalMatcher = new obvious::RandomNormalMatching(trials, epsThresh, sizeControlSet);
    break;
  case PDF:
    _PDFMatcher = new obvious::PDFMatching(trials, epsThresh, sizeControlSet, zhit, zphi, zshort, zmax, zrand, percentagePointsInC, rangemax, sigphi, sighit, lamshort, maxAngleDiff, maxAnglePenalty);
    break;
  case TSD:
    _TSD_PDFMatcher = new obvious::TSD_PDFMatching(_grid, trials, epsThresh, sizeControlSet, zhit, zphi, zshort, zmax, zrand, percentagePointsInC, rangemax, sigphi, sighit, lamshort, maxAngleDiff, maxAnglePenalty);
    break;
  default:
    ROS_ERROR_STREAM("Unknown registration mode " << _regMode << " use default = ICP" << std::endl);
    break;
  }

  _modelCoords  = NULL;
  _modelNormals = NULL;
  _maskM        = NULL;
  _rayCaster    = NULL;
  _scene        = NULL;
  _maskS        = NULL;


  /** Initialize member modules **/
  _lastPose         = new obvious::Matrix(3, 3);
  _rayCaster        = new obvious::RayCastPolar2D();
  _assigner         = new obvious::FlannPairAssignment(2);
  _filterDist       = new obvious::DistanceFilter(distFilterMax, distFilterMin, icpIterations - 10);
  _filterReciprocal = new obvious::ReciprocalFilter();
  _estimator        = new obvious::ClosedFormEstimator2D();

  //configure ICP
  _filterBounds = new obvious::OutOfBoundsFilter2D(grid->getMinX(), grid->getMaxX(), grid->getMinY(), grid->getMaxY());
  _assigner->addPreFilter(_filterBounds);
  _assigner->addPostFilter(_filterDist);
  _assigner->addPostFilter(_filterReciprocal);
  _icp = new obvious::Icp(_assigner, _estimator);
  _icp->setMaxRMS(0.0);
  _icp->setMaxIterations(icpIterations);
  _icp->setConvergenceCounter(icpIterations);

  _posePub = _nh->advertise<geometry_msgs::PoseStamped>(poseTopic, 1);
  _poseStamped.header.frame_id = tfBaseFrameId;
  _tf.frame_id_                = tfBaseFrameId;
  _tf.child_frame_id_          = _nameSpace + tfChildFrameId;

  _reverseScan = false;
}

ThreadLocalize::~ThreadLocalize()
{
//  // (debug)print grid
//  // todo: remove printing grid
//  double ratio = double(_grid.getCellsX())/double(_grid.getCellsY());
//  unsigned w = 600;
//  unsigned h = (unsigned int)(((double)w)/ratio);
//  unsigned char* image = new unsigned char[3 * w * h];
//  _grid.grid2ColorImage(image, w, h);
//  obvious::serializePPM("/tmp/image_tsd.ppm", image, w, h, true);


  delete _sensor;
  delete _RandomNormalMatcher;
  delete _PDFMatcher;
  delete _TSD_PDFMatcher;
  for(std::deque<sensor_msgs::LaserScan*>::iterator iter = _laserData.begin(); iter < _laserData.end(); iter++)
    delete *iter;
  _stayActive = false;
  _thread->join();
  _laserData.clear();
}

void ThreadLocalize::laserCallBack(const sensor_msgs::LaserScan& scan)
{
  if(!_initialized)
  {
    ROS_INFO_STREAM("Localizer(" << _nameSpace << ") received first scan. Initialize node...\n");
    this->init(scan);
    ROS_INFO_STREAM("Localizer(" << _nameSpace << ") initialized -> running...\n");
    return;
  }
  sensor_msgs::LaserScan* scanCopy = new sensor_msgs::LaserScan;
  *scanCopy = scan;
  _dataMutex.lock();
  _laserData.push_front(scanCopy);
  _dataMutex.unlock();
  this->unblock();
}

void ThreadLocalize::eventLoop(void)
{
  _sleepCond.wait(_sleepMutex);
  while(_stayActive)
  {

    if(!_laserData.size())
    {
      _sleepCond.wait(_sleepMutex);
    }

    _dataMutex.lock();

    _actualMeasurementStamp = _laserData.front()->header.stamp;

    vector<float> ranges = _laserData.front()->ranges;
    if(_reverseScan)
      std::reverse(ranges.begin(),ranges.end());

    _sensor->setRealMeasurementData(ranges);
    _sensor->setStandardMask();

    for(std::deque<sensor_msgs::LaserScan*>::iterator iter = _laserData.begin(); iter < _laserData.end(); iter++)
      delete *iter;
    _laserData.clear();
    _dataMutex.unlock();

    const unsigned int measurementSize = _sensor->getRealMeasurementSize();

    if(!_scene)   //first call, initialize buffers
    {
      _scene        = new double[measurementSize * 2];
      _maskS        = new bool[measurementSize];
      _modelCoords  = new double[measurementSize * 2];
      _modelNormals = new double[measurementSize * 2];
      _maskM        = new bool[measurementSize];
      *_lastPose    = _sensor->getTransformation();
    }

    // reconstruction
    unsigned int validModelPoints = _rayCaster->calcCoordsFromCurrentViewMask(&_grid, _sensor, _modelCoords, _modelNormals, _maskM);
    if(validModelPoints == 0)
    {
      ROS_ERROR_STREAM("Localizer(" << _nameSpace <<") error! Raycasting found no coordinates!\n");
      continue;
    }

    //get current scan
    const unsigned int validScenePoints = _sensor->dataToCartesianVectorMask(_scene, _maskS);
    //_sensor->dataToCartesianVector(_scene);

    /**
     *  Create Point Matrixes with structure [x1 y1; x2 y2; ..]
     *  M, N, and S are matrices that preserve the ray model of a laser scanner
     *  Xvalid matrices are matrices that do not preserve the ray model but contain only valid points
     */
    obvious::Matrix M(measurementSize, 2, _modelCoords);
    obvious::Matrix N(measurementSize, 2, _modelNormals);
    obvious::Matrix Mvalid = maskMatrix(&M, _maskM, measurementSize, validModelPoints);
    obvious::Matrix Nvalid = maskMatrix(&N, _maskM, measurementSize, validModelPoints);
    obvious::Matrix S(measurementSize, 2, _scene);
    obvious::Matrix Svalid = maskMatrix(&S, _maskS, measurementSize, validScenePoints);
    obvious::Matrix T(3, 3);

    T = doRegistration(_sensor, &M, &Mvalid, &N, &Nvalid, &S, &Svalid);  //3x3 Transformation Matrix

    /** analyze registration result */
    _tf.stamp_ = ros::Time::now();
    const bool regErrorT = isRegistrationError(&T, _trnsMax, _rotMax);
    if(regErrorT)
    {
      ROS_ERROR_STREAM("Localizer(" << _nameSpace << ") registration error! \n");
      sendNanTransform();
    }
    else //transformation valid -> transform sensor and publish new sensor pose
    {
      _sensor->transform(&T);
      obvious::Matrix curPose = _sensor->getTransformation();
      sendTransform(&curPose);
      /** Update MAP if necessary */
      if(this->isPoseChangeSignificant(_lastPose, &curPose))
      {
        *_lastPose = curPose;
        _mapper.queuePush(_sensor);
      }
    }
  }
}

void ThreadLocalize::init(const sensor_msgs::LaserScan& scan)
{
  double localXoffset    = 0.0;
  double localYoffset    = 0.0;
  double localYawOffset  = 0.0;
  double maxRange = 0.0;
  double minRange = 0.0;
  double lowReflectivityRange = 0.0;
  double footPrintWidth= 0.0;
  double footPrintHeight= 0.0;
  double footPrintXoffset= 0.0;

  ros::NodeHandle prvNh("~");

  prvNh.param<double>(_nameSpace + "local_offset_x"        , localXoffset        , 0.0);
  prvNh.param<double>(_nameSpace + "local_offset_y"        , localYoffset        , 0.0);
  prvNh.param<double>(_nameSpace + "local_offset_yaw"      , localYawOffset      , 0.0);
  prvNh.param<double>(_nameSpace + "max_range"             , maxRange            , 30.0);
  prvNh.param<double>(_nameSpace + "min_range"             , minRange            , 0.001);
  prvNh.param<double>(_nameSpace + "low_reflectivity_range", lowReflectivityRange, 2.0);
  prvNh.param<double>(_nameSpace + "footprint_width"       , footPrintWidth      , 1.0);
  prvNh.param<double>(_nameSpace + "footprint_height"      , footPrintHeight     , 1.0);
  prvNh.param<double>(_nameSpace + "footprint_x_offset"    , footPrintXoffset    , 0.28);

  const double phi    = localYawOffset;
  const double startX = _gridWidth * 0.5 + _xOffset + localXoffset;
  const double startY = _gridWidth * 0.5 + _yOffset + localYoffset;
  double tf[9]  = {std::cos(phi), -std::sin(phi), startX,
      std::sin(phi),  std::cos(phi), startY,
      0,              0,      1};
  obvious::Matrix Tinit(3, 3);
  Tinit.setData(tf);

  double inc = scan.angle_increment;
  double angle_min = scan.angle_min;
  vector<float> ranges = scan.ranges;

  if(scan.angle_increment<0.0 && scan.angle_min>0)
  {
    _reverseScan = true;
    inc = -inc;
    angle_min = -angle_min;
    std::reverse(ranges.begin(),ranges.end());
  }
  _sensor = new obvious::SensorPolar2D(ranges.size(), inc, angle_min, maxRange, minRange, lowReflectivityRange);
  _sensor->setRealMeasurementData(ranges, 1.0);

  _sensor->setStandardMask();
  _sensor->transform(&Tinit);
  obfloat t[2] = {startX + footPrintXoffset, startY};
  if(!_grid.freeFootprint(t, footPrintWidth, footPrintHeight))
    ROS_ERROR_STREAM("Localizer(" << _nameSpace << ") warning! Footprint could not be freed!\n");
  if(!_mapper.initialized())
    _mapper.initPush(_sensor);
  _initialized = true;
  this->unblock();
}

obvious::Matrix ThreadLocalize::doRegistration(obvious::SensorPolar2D* sensor,
    obvious::Matrix* M,
    obvious::Matrix* Mvalid,
    obvious::Matrix* N,
    obvious::Matrix* Nvalid,
    obvious::Matrix* S,
    obvious::Matrix* Svalid
)
{
//  const unsigned int measurementSize = sensor->getRealMeasurementSize();
  obvious::Matrix T44(4, 4);
  T44.setIdentity();
  obvious::Matrix T(3,3);

#ifdef TRACE
  obvious::Matrix T_old(3,3);
#endif

  // RANSAC pre-registration (rough)
  switch(_regMode)
  {
  case ICP:
    // no pre-registration
    break;
  case EXP:
    // todo: check normals N for matching function (Daniel Ammon, Tobias Fink)
    T = _RandomNormalMatcher->match(M, _maskM, NULL, S, _maskS, obvious::deg2rad(_ranPhiMax), _trnsMax, sensor->getAngularResolution());
    T44(0, 0) = T(0, 0);
    T44(0, 1) = T(0, 1);
    T44(0, 3) = T(0, 2);
    T44(1, 0) = T(1, 0);
    T44(1, 1) = T(1, 1);
    T44(1, 3) = T(1, 2);
    break;
  case PDF:
    // todo: check normals N for matching function (Daniel Ammon, Tobias Fink)
    T = _PDFMatcher->match(M, _maskM, NULL, S, _maskS, obvious::deg2rad(_ranPhiMax), _trnsMax, sensor->getAngularResolution());
    T44(0, 0) = T(0, 0);
    T44(0, 1) = T(0, 1);
    T44(0, 3) = T(0, 2);
    T44(1, 0) = T(1, 0);
    T44(1, 1) = T(1, 1);
    T44(1, 3) = T(1, 2);
    break;
  case TSD:
    // todo: check normals N for matching function (Daniel Ammon, Tobias Fink)
    T = _TSD_PDFMatcher->match(sensor->getTransformation(), M, _maskM, NULL, S, _maskS, obvious::deg2rad(_ranPhiMax), _trnsMax, sensor->getAngularResolution());
    T44(0, 0) = T(0, 0);
    T44(0, 1) = T(0, 1);
    T44(0, 3) = T(0, 2);
    T44(1, 0) = T(1, 0);
    T44(1, 1) = T(1, 1);
    T44(1, 3) = T(1, 2);
    break;
  default:
    // no pre-registration
    break;
  }

  _icp->reset();
  obvious::Matrix P = sensor->getTransformation();
  _filterBounds->setPose(&P);

  _icp->setModel(Mvalid, Nvalid);
  _icp->setScene(Svalid);
  double rms = 0.0;
  unsigned int pairs = 0;
  unsigned int it = 0;
  _icp->iterate(&rms, &pairs, &it, &T44);
  T = _icp->getFinalTransformation();
  return T;
}

bool ThreadLocalize::isRegistrationError(obvious::Matrix* T, const double trnsMax, const double rotMax)
{
  const double deltaX = (*T)(0, 2);
  const double deltaY = (*T)(1, 2);
  const double trnsAbs = std::sqrt(deltaX * deltaX + deltaY * deltaY);
  const double deltaPhi = this->calcAngle(T);
  return (trnsAbs > trnsMax) || (std::abs(std::sin(deltaPhi)) > rotMax);
}

void ThreadLocalize::sendTransform(obvious::Matrix* T)
{
  const double curTheta = this->calcAngle(T);
  const double posX = (*T)(0, 2) + _gridOffSetX;
  const double posY = (*T)(1, 2) + _gridOffSetY;
  _poseStamped.header.stamp = _actualMeasurementStamp;
  _poseStamped.pose.position.x = posX;
  _poseStamped.pose.position.y = posY;
  _poseStamped.pose.position.z = 0.0;
  tf::Quaternion quat;
  quat.setEuler(0.0, 0.0, curTheta);
  _poseStamped.pose.orientation.w = quat.w();
  _poseStamped.pose.orientation.x = quat.x();
  _poseStamped.pose.orientation.y = quat.y();
  _poseStamped.pose.orientation.z = quat.z();

  _tf.stamp_ = _actualMeasurementStamp;
  _tf.setOrigin(tf::Vector3(posX, posY, 0.0));
  _tf.setRotation(quat);

  _posePub.publish(_poseStamped);
  _tfBroadcaster.sendTransform(_tf);
}

void ThreadLocalize::sendNanTransform()
{
  _poseStamped.header.stamp = ros::Time::now();
  _poseStamped.pose.position.x = NAN;
  _poseStamped.pose.position.y = NAN;
  _poseStamped.pose.position.z = NAN;
  tf::Quaternion quat;
  quat.setEuler(NAN, NAN, NAN);
  _poseStamped.pose.orientation.w = quat.w();
  _poseStamped.pose.orientation.x = quat.x();
  _poseStamped.pose.orientation.y = quat.y();
  _poseStamped.pose.orientation.z = quat.z();

  _tf.stamp_ = ros::Time::now();
  _tf.setOrigin(tf::Vector3(NAN, NAN, NAN));
  _tf.setRotation(quat);

  _posePub.publish(_poseStamped);
  _tfBroadcaster.sendTransform(_tf);
}

double ThreadLocalize::calcAngle(obvious::Matrix* T)
{
  double angle          = 0.0;
  const double ARCSIN   = asin((*T)(1,0));
  const double ARCSINEG = asin((*T)(0,1));
  const double ARCOS    = acos((*T)(0,0));
  if((ARCSIN > 0.0) && (ARCSINEG < 0.0))
    angle = ARCOS;
  else if((ARCSIN < 0.0) && (ARCSINEG > 0.0))
    angle = 2.0 * M_PI - ARCOS;
  return(angle);
}

bool ThreadLocalize::isPoseChangeSignificant(obvious::Matrix* lastPose, obvious::Matrix* curPose)
{
  const double deltaX   = (*curPose)(0, 2) - (*lastPose)(0, 2);
  const double deltaY   = (*curPose)(1, 2) - (*lastPose)(1, 2);
  double deltaPhi       = this->calcAngle(curPose) - this->calcAngle(lastPose);
  deltaPhi              = fabs(sin(deltaPhi));
  const double trnsAbs  = sqrt(deltaX * deltaX + deltaY * deltaY);
  return (deltaPhi > ROT_MIN) || (trnsAbs > TRNS_MIN);
}

obvious::Matrix ThreadLocalize::maskMatrix(obvious::Matrix* Mat, bool* mask, unsigned int maskSize, unsigned int validPoints)
{
  assert(Mat->getRows() == maskSize);
  assert(Mat->getCols() == 2);
  obvious::Matrix retMat(validPoints, 2);
  unsigned int cnt = 0;
  for(unsigned int i = 0; i < maskSize; i++)
  {
    if(mask[i])
    {
      retMat(cnt, 0) = (*Mat)(i, 0);
      retMat(cnt, 1) = (*Mat)(i, 1);
      cnt++;
    }
  }
  return retMat;
}

//toDo: maybe obsolete with pca matching, definitely not nice
void ThreadLocalize::reduceResolution(bool* const maskIn, const obvious::Matrix* matIn, bool* const maskOut, obvious::Matrix* matOut,
    const unsigned int pointsIn, const unsigned int pointsOut, const unsigned int reductionFactor)
{
  assert(pointsIn > pointsOut);
  //fixme we only support scan with even number of points like 1080. if a scan has 1081 points is not usable for subsampling here!
  const unsigned int factor = pointsIn / pointsOut;
  assert(factor == reductionFactor);

  unsigned int cnt = 0;
  for(unsigned int i = 0; i < pointsIn; i++)
  {
    if(!(i % factor)) // i % factor == 0
    {
      cnt++;
      if (maskIn[i]) {
        maskOut[i/factor] = true;
        (*matOut)(i/factor, 0) = (*matIn)(i, 0);
        (*matOut)(i/factor, 1) = (*matIn)(i, 1);
      }
      else
      {
        maskOut[i/factor] = false;
      }
    }
  }
  assert(cnt == pointsOut);
}

} /* namespace ohm_tsd_slam */
