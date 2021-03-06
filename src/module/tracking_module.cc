#include "tracking_module.h"

#include "util/tf_util.h"
#include "util/math_util.h"
#include <omp.h>

using namespace std;

namespace omni_slam
{
namespace module
{

TrackingModule::TrackingModule(std::unique_ptr<feature::Detector> &detector, std::unique_ptr<feature::Tracker> &tracker, std::unique_ptr<odometry::FivePoint> &checker, int minFeaturesRegion, int maxFeaturesRegion)
    : detector_(std::move(detector)),
    tracker_(std::move(tracker)),
    fivePointChecker_(std::move(checker)),
    minFeaturesRegion_(minFeaturesRegion),
    maxFeaturesRegion_(maxFeaturesRegion)
{
}

TrackingModule::TrackingModule(std::unique_ptr<feature::Detector> &&detector, std::unique_ptr<feature::Tracker> &&tracker, std::unique_ptr<odometry::FivePoint> &&checker, int minFeaturesRegion, int maxFeaturesRegion)
    : TrackingModule(detector, tracker, checker, minFeaturesRegion, maxFeaturesRegion)
{
}

void TrackingModule::Update(std::unique_ptr<data::Frame> &frame)
{
    frames_.push_back(std::move(frame));

    int imsize = max(frames_.back()->GetImage().rows, frames_.back()->GetImage().cols);
    if (frameNum_ == 0)
    {
        tracker_->Init(*frames_.back());
        lastKeyframe_ = tracker_->GetLastKeyframe();

        visualization_.Init(frames_.back()->GetImage().size(), landmarks_.size());

        frameNum_++;
        return;
    }

    lastKeyframe_ = tracker_->GetLastKeyframe();
    vector<double> trackErrors;
    int tracks = tracker_->Track(landmarks_, *frames_.back(), trackErrors);
    if (fivePointChecker_ && tracks > 0)
    {
        Matrix3d E;
        std::vector<int> inlierIndices;
        fivePointChecker_->ComputeE(landmarks_, *lastKeyframe_, *frames_.back(), E, inlierIndices);
        std::unordered_set<int> inlierSet(inlierIndices.begin(), inlierIndices.end());
        for (int i = 0; i < landmarks_.size(); i++)
        {
            if (landmarks_[i].IsObservedInFrame(frames_.back()->GetID()) && inlierSet.find(i) == inlierSet.end())
            {
                landmarks_[i].RemoveLastObservation();
            }
        }
        if (frames_.back()->HasStereoImage())
        {
            Matrix3d E;
            std::vector<int> inlierIndices;
            fivePointChecker_->ComputeE(landmarks_, *lastKeyframe_, *frames_.back(), E, inlierIndices, true);
            std::unordered_set<int> inlierSet(inlierIndices.begin(), inlierIndices.end());
            for (int i = 0; i < landmarks_.size(); i++)
            {
                if (landmarks_[i].GetStereoObservationByFrameID(frames_.back()->GetID()) != nullptr && inlierSet.find(i) == inlierSet.end())
                {
                    landmarks_[i].RemoveLastStereoObservation();
                }
            }
        }
    }

    int i = 0;
    int numGood = 0;
    regionCount_.clear();
    regionLandmarks_.clear();
    stats_.trackLengths.resize(landmarks_.size(), 0);
    for (data::Landmark& landmark : landmarks_)
    {
        const data::Feature *obs = landmark.GetObservationByFrameID(frames_.back()->GetID());
        if (obs != nullptr)
        {
            double x = obs->GetKeypoint().pt.x - frames_.back()->GetImage().cols / 2. + 0.5;
            double y = obs->GetKeypoint().pt.y - frames_.back()->GetImage().rows / 2. + 0.5;
            double r = sqrt(x * x + y * y) / imsize;
            double t = util::MathUtil::FastAtan2(y, x);
            vector<double>::const_iterator ri = upper_bound(feature::Region::rs.begin(), feature::Region::rs.end(), r);
            vector<double>::const_iterator ti = upper_bound(feature::Region::ts.begin(), feature::Region::ts.end(), t);
            int rinx = min((int)(ri - feature::Region::rs.begin()), (int)(feature::Region::rs.size() - 1)) - 1;
            int tinx = min((int)(ti - feature::Region::ts.begin()), (int)(feature::Region::ts.size() - 1)) - 1;
            regionCount_[{rinx, tinx}]++;
            regionLandmarks_[{rinx, tinx}].push_back(&landmark);

            const data::Feature *obsPrevFrame = landmark.GetObservationByFrameID((*next(frames_.rbegin()))->GetID());
            const data::Feature *obsPrev = landmark.GetObservationByFrameID(lastKeyframe_->GetID());
            if (obsPrev != nullptr)
            {
                Vector2d pixelGnd;
                if (frames_.back()->HasPose() && landmark.HasGroundTruth())
                {
                    if (frames_.back()->GetCameraModel().ProjectToImage(util::TFUtil::WorldFrameToCameraFrame(util::TFUtil::TransformPoint(frames_.back()->GetInversePose(), landmark.GetGroundTruth())), pixelGnd))
                    {
                        Vector2d pixel;
                        pixel << obs->GetKeypoint().pt.x, obs->GetKeypoint().pt.y;
                        double error = (pixel - pixelGnd).norm();

                        if (obsPrevFrame != nullptr)
                        {
                            visualization_.AddTrack(cv::Point2f(pixelGnd(0), pixelGnd(1)), obsPrevFrame->GetKeypoint().pt, obs->GetKeypoint().pt, error, i);
                        }

                        double xg = pixelGnd(0) - frames_.back()->GetImage().cols / 2. + 0.5;
                        double yg = pixelGnd(1) - frames_.back()->GetImage().rows / 2. + 0.5;
                        double rg = sqrt(xg * xg + yg * yg) / imsize;

                        Vector2d pixelPrev;
                        pixelPrev << obsPrev->GetKeypoint().pt.x, obsPrev->GetKeypoint().pt.y;
                        Vector3d flow;
                        flow = (pixel - pixelPrev).homogeneous().normalized();
                        Vector3d ray;
                        frames_.back()->GetCameraModel().UnprojectToBearing(pixel, ray);
                        Vector3d rayGnd;
                        frames_.back()->GetCameraModel().UnprojectToBearing(pixelGnd, rayGnd);
                        Vector3d rayPrev;
                        lastKeyframe_->GetCameraModel().UnprojectToBearing(pixelPrev, rayPrev);
                        Vector3d rayGndPrev = rayPrev;
                        double prevError = 0;
                        Vector3d flowGnd;
                        flowGnd << 0, 0, 1;
                        Vector2d pixelGndPrev;
                        if (lastKeyframe_->GetCameraModel().ProjectToImage(util::TFUtil::WorldFrameToCameraFrame(util::TFUtil::TransformPoint(lastKeyframe_->GetInversePose(), landmark.GetGroundTruth())), pixelGndPrev))
                        {
                            prevError = (pixelPrev - pixelGndPrev).norm();
                            flowGnd = (pixelGnd - pixelGndPrev).homogeneous().normalized();
                            lastKeyframe_->GetCameraModel().UnprojectToBearing(pixelGndPrev, rayGndPrev);
                        }
                        double angularError = acos(flow.dot(flowGnd));
                        double bearingError = acos(ray.normalized().dot(rayGnd.normalized()));
                        double bearingErrorPrev = acos(rayPrev.normalized().dot(rayGndPrev.normalized()));
                        stats_.radialErrors.emplace_back(vector<double>{rg, error, error - prevError, angularError, bearingError - bearingErrorPrev});
                        stats_.frameErrors.emplace_back(vector<double>{(double)landmark.GetNumObservations() - 1, (double)i, rg, error, bearingError});
                        stats_.successRadDists.emplace_back(vector<double>{rg, (double)frameNum_});
                    }
                }
                else
                {
                    if (obsPrevFrame != nullptr)
                    {
                        visualization_.AddTrack(obsPrevFrame->GetKeypoint().pt, obs->GetKeypoint().pt, i);
                    }
                }
            }
            stats_.trackLengths[i]++;
            numGood++;
        }
        else
        {
            const data::Feature *obs = landmark.GetObservationByFrameID(lastKeyframe_->GetID());
            if (obs != nullptr) // Failed in current frame
            {

                Vector2d pixelGnd;
                if (frames_.back()->GetCameraModel().ProjectToImage(util::TFUtil::WorldFrameToCameraFrame(util::TFUtil::TransformPoint(frames_.back()->GetInversePose(), landmark.GetGroundTruth())), pixelGnd))
                {
                    double x = pixelGnd(0) - frames_.back()->GetImage().cols / 2. + 0.5;
                    double y = pixelGnd(1) - frames_.back()->GetImage().rows / 2. + 0.5;
                    double r = sqrt(x * x + y * y) / imsize;
                    stats_.failureRadDists.emplace_back(vector<double>{r, (double)frameNum_});
                }
                stats_.trackLengths.push_back(landmark.GetNumObservations() - 1);
            }
        }
        i++;
    }

    stats_.frameTrackCounts.emplace_back(vector<int>{frameNum_, numGood});

    Prune();

    (*next(frames_.rbegin()))->CompressImages();

    frameNum_++;
}

void TrackingModule::Redetect()
{
    if (tracker_->GetLastKeyframe() != frames_.back().get())
    {
        return;
    }
    int imsize = max(frames_.back()->GetImage().rows, frames_.back()->GetImage().cols);
    #pragma omp parallel for collapse(2)
    for (int i = 0; i < feature::Region::rs.size() - 1; i++)
    {
        for (int j = 0; j < feature::Region::ts.size() - 1; j++)
        {
            if (regionCount_.find({i, j}) == regionCount_.end() || regionCount_.at({i, j}) < minFeaturesRegion_)
            {
                detector_->DetectInRadialRegion(*frames_.back(), landmarks_, feature::Region::rs[i] * imsize, feature::Region::rs[i+1] * imsize, feature::Region::ts[j], feature::Region::ts[j+1]);
            }
        }
    }
}

void TrackingModule::Prune()
{
    #pragma omp parallel for collapse(2)
    for (int i = 0; i < feature::Region::rs.size() - 1; i++)
    {
        for (int j = 0; j < feature::Region::ts.size() - 1; j++)
        {
            if (regionCount_.find({i, j}) != regionCount_.end() && regionCount_.at({i, j}) > maxFeaturesRegion_)
            {
                sort(regionLandmarks_.at({i, j}).begin(), regionLandmarks_.at({i, j}).end(),
                        [](const data::Landmark *a, const data::Landmark *b) -> bool
                        {
                            return a->GetNumObservations() > b->GetNumObservations();
                        });
                for (int c = 0; c < regionCount_.at({i, j}) - maxFeaturesRegion_; c++)
                {
                    regionLandmarks_.at({i, j})[c]->RemoveLastObservation();
                }
            }
        }
    }
}

std::vector<data::Landmark>& TrackingModule::GetLandmarks()
{
    return landmarks_;
}

std::vector<std::unique_ptr<data::Frame>>& TrackingModule::GetFrames()
{
    return frames_;
}

const data::Frame* TrackingModule::GetLastKeyframe()
{
    return lastKeyframe_;
}

TrackingModule::Stats& TrackingModule::GetStats()
{
    return stats_;
}

void TrackingModule::Visualize(cv::Mat &base_img)
{
    visualization_.Draw(base_img);
}

void TrackingModule::Visualization::Init(cv::Size img_size, int num_colors)
{
    visMask_ = cv::Mat::zeros(img_size, CV_8UC3);
    curMask_ = cv::Mat::zeros(img_size, CV_8UC3);
    cv::RNG rng(123);
    for (int i = 0; i < max(num_colors, 100); i++)
    {
        colors_.emplace_back(rng.uniform(10, 200), rng.uniform(10, 200), rng.uniform(10, 200));
    }
}

void TrackingModule::Visualization::AddTrack(cv::Point2f gnd, cv::Point2f prev, cv::Point2f cur, double error, int index)
{
    int maxerror = min(visMask_.rows, visMask_.cols) / 100;
    cv::Scalar color(0, (int)(255 * (1 - error / maxerror)), (int)(255 * (error / maxerror)));
    cv::line(visMask_, prev, cur, color, 1);
    cv::circle(curMask_, cur, 1, color, -1);
    cv::circle(curMask_, gnd, 3, colors_[index % colors_.size()], -1);
}

void TrackingModule::Visualization::AddTrack(cv::Point2f prev, cv::Point2f cur, int index)
{
    cv::Scalar color(255, 0, 0);
    cv::line(visMask_, prev, cur, color, 1);
    cv::circle(curMask_, cur, 1, color, -1);
}

void TrackingModule::Visualization::Draw(cv::Mat &img)
{
    if (img.channels() == 1)
    {
        cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);
    }
    curMask_.copyTo(img, curMask_);
    cv::addWeighted(img, 1, visMask_, trackOpacity_, 0, img);
    curMask_ = cv::Mat::zeros(img.size(), CV_8UC3);
    visMask_ *= trackFade_;
}

}
}
