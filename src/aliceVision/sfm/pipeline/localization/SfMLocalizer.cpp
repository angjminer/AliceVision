// This file is part of the AliceVision project.
// Copyright (c) 2016 AliceVision contributors.
// Copyright (c) 2012 openMVG contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "SfMLocalizer.hpp"
#include <aliceVision/sfm/BundleAdjustmentCeres.hpp>
#include <aliceVision/multiview/resection/ResectionKernel.hpp>
#include <aliceVision/multiview/resection/P3PSolver.hpp>
#include <aliceVision/robustEstimation/ACRansac.hpp>
#include <aliceVision/robustEstimation/ACRansacKernelAdaptator.hpp>
#include <aliceVision/robustEstimation/LORansac.hpp>
#include <aliceVision/robustEstimation/LORansacKernelAdaptor.hpp>
#include <aliceVision/robustEstimation/ScoreEvaluator.hpp>
#include <aliceVision/config.hpp>

namespace aliceVision {
namespace sfm {

struct ResectionSquaredResidualError 
{
  // Compute the residual of the projection distance(pt2D, Project(P,pt3D))
  // Return the squared error
  static double Error(const Mat34& P, const Vec2& pt2D, const Vec3& pt3D)
  {
    const Vec2 x = Project(P, pt3D);
    return (x - pt2D).squaredNorm();
  }
};

bool SfMLocalizer::Localize(const Pair& imageSize,
                            const camera::IntrinsicBase* optionalIntrinsics,
                            ImageLocalizerMatchData& resectionData,
                            geometry::Pose3& pose,
                            robustEstimation::ERobustEstimator estimator)
{
  // --
  // Compute the camera pose (resectioning)
  // --
  Mat34 P;
  resectionData.vec_inliers.clear();

  // Setup the admissible upper bound residual error
  const double precision =
    resectionData.error_max == std::numeric_limits<double>::infinity() ?
    std::numeric_limits<double>::infinity() :
    Square(resectionData.error_max);

  std::size_t minimumSamples = 0;
  const camera::Pinhole* pinholeCam = dynamic_cast<const camera::Pinhole*>(optionalIntrinsics);

  if (pinholeCam == nullptr || !pinholeCam->isValid())
  {
    //--
    // Classic resection (try to compute the entire P matrix)
    typedef aliceVision::resection::kernel::SixPointResectionSolver SolverType;
    minimumSamples = SolverType::MINIMUM_SAMPLES;

    typedef aliceVision::robustEstimation::ACKernelAdaptorResection<
      SolverType, ResectionSquaredResidualError, aliceVision::robustEstimation::UnnormalizerResection, Mat34>
      KernelType;

    KernelType kernel(resectionData.pt2D, imageSize.first, imageSize.second, resectionData.pt3D);
    // Robust estimation of the Projection matrix and its precision
    const std::pair<double,double> ACRansacOut =
      aliceVision::robustEstimation::ACRANSAC(kernel, resectionData.vec_inliers, resectionData.max_iteration, &P, precision, true);
    // Update the upper bound precision of the model found by AC-RANSAC
    resectionData.error_max = ACRansacOut.first;
  }
  else
  {
    // undistort the points if the camera has a distortion model
    Mat pt2Dundistorted;
    const bool hasDistortion = pinholeCam->have_disto();
    if(hasDistortion)
    {
      const std::size_t numPts = resectionData.pt2D.cols();
      pt2Dundistorted = Mat2X(2, numPts);
      for(std::size_t iPoint = 0; iPoint < numPts; ++iPoint)
      {
        pt2Dundistorted.col(iPoint) = pinholeCam->get_ud_pixel(resectionData.pt2D.col(iPoint));
      }
    }

    switch(estimator)
    {
      case robustEstimation::ERobustEstimator::ACRANSAC:
      {
        //--
        // Since K calibration matrix is known, compute only [R|t]
        typedef aliceVision::resection::P3PSolver SolverType;
        minimumSamples = SolverType::MINIMUM_SAMPLES;

        typedef aliceVision::robustEstimation::ACKernelAdaptorResection_K<
                SolverType, ResectionSquaredResidualError,
                aliceVision::robustEstimation::UnnormalizerResection, Mat34> KernelType;

        // otherwise we just pass the input points
        KernelType kernel = KernelType(hasDistortion ? pt2Dundistorted : resectionData.pt2D, resectionData.pt3D, pinholeCam->K());

        // Robust estimation of the Projection matrix and its precision
        const std::pair<double, double> ACRansacOut =
                aliceVision::robustEstimation::ACRANSAC(kernel, resectionData.vec_inliers, resectionData.max_iteration, &P, precision, true);
        // Update the upper bound precision of the model found by AC-RANSAC
        resectionData.error_max = ACRansacOut.first;
        break;
      }

      case robustEstimation::ERobustEstimator::LORANSAC:
      {

        // just a safeguard
        if(resectionData.error_max == std::numeric_limits<double>::infinity())
        {
          // switch to a default value
          resectionData.error_max = 4.0;
          ALICEVISION_LOG_DEBUG("LORansac: error was set to infinity, a default value of " 
                  << resectionData.error_max << " is going to be used");
        }

        // use the P3P solver for generating the model
        typedef aliceVision::resection::P3PSolver SolverType;
        minimumSamples = SolverType::MINIMUM_SAMPLES;
        // use the six point algorithm as Least square solution to refine the model
        typedef aliceVision::resection::kernel::SixPointResectionSolver SolverLSType;

        typedef aliceVision::robustEstimation::KernelAdaptorResectionLORansac_K<SolverType,
                ResectionSquaredResidualError,
                aliceVision::robustEstimation::UnnormalizerResection,
                SolverLSType,
                Mat34> KernelType;

        // otherwise we just pass the input points
        KernelType kernel = KernelType(hasDistortion ? pt2Dundistorted : resectionData.pt2D, resectionData.pt3D, pinholeCam->K());

        // this is just stupid and ugly, the threshold should be always give as pixel
        // value, the scorer should be not aware of the fact that we treat squared errors
        // and normalization inside the kernel
        // @todo refactor, maybe move scorer directly inside the kernel
        const double threshold = resectionData.error_max * resectionData.error_max * (kernel.normalizer2()(0, 0) * kernel.normalizer2()(0, 0));
        robustEstimation::ScoreEvaluator<KernelType> scorer(threshold);
        P = robustEstimation::LO_RANSAC(kernel, scorer, &resectionData.vec_inliers);
        break;
      }

      default:
        throw std::runtime_error("[SfMLocalizer::localize] Only ACRansac and LORansac are supported!");
    }
  }

  const bool resection = robustEstimation::hasStrongSupport(resectionData.vec_inliers, resectionData.vec_descType, minimumSamples);

  if(!resection)
  {
    ALICEVISION_LOG_DEBUG("Resection status is false:\n"
                          "\t- resection_data.vec_inliers.size() = " << resectionData.vec_inliers.size() << "\n"
                          "\t- minimumSamples = " << minimumSamples);
  }

  if(resection)
  {
    resectionData.projection_matrix = P;
    Mat3 K, R;
    Vec3 t;
    KRt_From_P(P, &K, &R, &t);
    pose = geometry::Pose3(R, -R.transpose() * t);
  }

  ALICEVISION_LOG_INFO("Robust Resection information:\n"
    "\t- resection status: " << resection << "\n"
    "\t- threshold (error max): " << resectionData.error_max << "\n"
    "\t- # points used for resection: " << resectionData.pt2D.cols() << "\n"
    "\t- # points validated by robust resection: " << resectionData.vec_inliers.size());

  return resection;
}

bool SfMLocalizer::RefinePose(camera::IntrinsicBase* intrinsics,
                              geometry::Pose3& pose,
                              const ImageLocalizerMatchData& matchingData,
                              bool refinePose,
                              bool refineIntrinsic)
{
  // Setup a tiny SfM scene with the corresponding 2D-3D data
  sfmData::SfMData tinyScene;

  // view
  std::shared_ptr<sfmData::View> view = std::make_shared<sfmData::View>("", 0, 0, 0);
  tinyScene.views.insert(std::make_pair(0, view));

  // pose
  tinyScene.setPose(*view, sfmData::CameraPose(pose));

  // intrinsic (the shared_ptr does not take the ownership, will not release the input pointer)
  std::shared_ptr<camera::IntrinsicBase> localIntrinsics(intrinsics->clone());
  tinyScene.intrinsics[0] = localIntrinsics;

  // structure data (2D-3D correspondences)
  for(std::size_t i = 0; i < matchingData.vec_inliers.size(); ++i)
  {
    const std::size_t idx = matchingData.vec_inliers[i];
    sfmData::Landmark landmark;
    landmark.X = matchingData.pt3D.col(idx);
    landmark.observations[0] = sfmData::Observation(matchingData.pt2D.col(idx), UndefinedIndexT);
    tinyScene.structure[i] = std::move(landmark);
  }

  BundleAdjustmentCeres bundle_adjustment_obj;
  BA_Refine refineOptions = BA_REFINE_NONE;

  if(refinePose)
    refineOptions |= BA_REFINE_ROTATION | BA_REFINE_TRANSLATION;
  if(refineIntrinsic)
    refineOptions |= BA_REFINE_INTRINSICS_ALL;

  const bool baStatus = bundle_adjustment_obj.Adjust(tinyScene, refineOptions);

  if(!baStatus)
    return false;

  pose = tinyScene.getPose(*view).getTransform();

  if(refineIntrinsic)
    intrinsics->assign(*localIntrinsics);

  return true;
}

} // namespace sfm
} // namespace aliceVision
