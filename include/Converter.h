/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef CONVERTER_H
#define CONVERTER_H

#include<opencv2/core/core.hpp>

#include<Eigen/Dense>
#include"Thirdparty/g2o/g2o/types/types_six_dof_expmap.h"
#include"Thirdparty/g2o/g2o/types/types_seven_dof_expmap.h"

namespace ORB_SLAM2
{

class Converter
{
public:
    static cv::Mat toCvMatInverse(const cv::Mat &T12);//return T21 fast using Isometry3d's property
    
    static Eigen::Isometry3d toIsometry3d(const cv::Mat &cvMat4);//transform cv::Mat(4,4,CV_32F) to Eigen::Isometry3d
    //created by zzh over.
    
    static std::vector<cv::Mat> toDescriptorVector(const cv::Mat &Descriptors);

    static g2o::SE3Quat toSE3Quat(const cv::Mat &cvT);//transform cv::Mat(4,4,float) to Eigen::Matrix<double,3,3>(R) && <3,1>(t), then call g2o::SE3Quat(R,t)
    static g2o::SE3Quat toSE3Quat(const g2o::Sim3 &gSim3);

    static cv::Mat toCvMat(const g2o::SE3Quat &SE3);//transform to cv::Mat(4,4,float)
    static cv::Mat toCvMat(const g2o::Sim3 &Sim3);//get R,t,s from g2o::Sim3 and transform them to cv::Mat(4,4,CV_32F)(S)
    static cv::Mat toCvMat(const Eigen::Matrix<double,4,4> &m);//transform to cv::Mat(4,4,CV_32F)(T/S)
    static cv::Mat toCvMat(const Eigen::Matrix3d &m);//transform to cv::Mat(3,3,CV_32F)
    static cv::Mat toCvMat(const Eigen::Matrix<double,3,1> &m);//transform Eigen::Matrix<double,3,1> to cv::Mat(3,1,CV_32F)(P/X)
    static cv::Mat toCvSE3(const Eigen::Matrix<double,3,3> &R, const Eigen::Matrix<double,3,1> &t);//transform Eigen::Matrix3d(R) && Eigen::Vector3d(t) to cv::Mat(4,4,float)(T/S(here R=sR'))

    static Eigen::Matrix<double,3,1> toVector3d(const cv::Mat &cvVector);//transform cv::Mat(3,1,CV_32F) to Eigen::Matrix<double,3,1>
    static Eigen::Matrix<double,3,1> toVector3d(const cv::Point3f &cvPoint);
    static Eigen::Matrix<double,3,3> toMatrix3d(const cv::Mat &cvMat3);//transform cv::Mat(3,3,CV_32F) to Eigen::Matrix<double,3,3>
    
    static std::vector<float> toQuaternion(const cv::Mat &M);
};

}// namespace ORB_SLAM

#endif // CONVERTER_H
