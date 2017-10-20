﻿/**
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

#include "Optimizer.h"

#include "Thirdparty/g2o/g2o/core/block_solver.h"
#include "Thirdparty/g2o/g2o/core/optimization_algorithm_levenberg.h"
#include "Thirdparty/g2o/g2o/solvers/linear_solver_eigen.h"
#include "Thirdparty/g2o/g2o/types/types_six_dof_expmap.h"
#include "Thirdparty/g2o/g2o/core/robust_kernel_impl.h"
#include "Thirdparty/g2o/g2o/solvers/linear_solver_dense.h"
#include "Thirdparty/g2o/g2o/types/types_seven_dof_expmap.h"

#include<Eigen/StdVector>

#include "Converter.h"

#include<mutex>

namespace ORB_SLAM2
{


void Optimizer::GlobalBundleAdjustemnt(Map* pMap, int nIterations, bool* pbStopFlag, const unsigned long nLoopKF, const bool bRobust)
{
    vector<KeyFrame*> vpKFs = pMap->GetAllKeyFrames();
    vector<MapPoint*> vpMP = pMap->GetAllMapPoints();
    BundleAdjustment(vpKFs,vpMP,nIterations,pbStopFlag, nLoopKF, bRobust);
}


void Optimizer::BundleAdjustment(const vector<KeyFrame *> &vpKFs, const vector<MapPoint *> &vpMP,
                                 int nIterations, bool* pbStopFlag, const unsigned long nLoopKF, const bool bRobust)
{
    vector<bool> vbNotIncludedMP;//true means this MP can not be used to optimize some KFs' Pose/is not added into optimizer/is not optimized
    vbNotIncludedMP.resize(vpMP.size());

    g2o::SparseOptimizer optimizer;
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;//6 KFs' se3 Pose && 3 MPs' pos

    linearSolver = new g2o::LinearSolverEigen<g2o::BlockSolver_6_3::PoseMatrixType>();//sparse Cholesky solver

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);//LM method
    optimizer.setAlgorithm(solver);

    if(pbStopFlag)//if mbStopGBA exists
        optimizer.setForceStopFlag(pbStopFlag);//_forceStopFlag=&mbStopGBA

    long unsigned int maxKFid = 0;

    // Set KeyFrame vertices
    for(size_t i=0; i<vpKFs.size(); i++)
    {
        KeyFrame* pKF = vpKFs[i];
        if(pKF->isBad())
            continue;
        g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
        vSE3->setEstimate(Converter::toSE3Quat(pKF->GetPose()));
        vSE3->setId(pKF->mnId);
        vSE3->setFixed(pKF->mnId==0);//GBA fix id0 KF, same in localBA
        optimizer.addVertex(vSE3);
        if(pKF->mnId>maxKFid)
            maxKFid=pKF->mnId;
    }

    const float thHuber2D = sqrt(5.99);//chi2(0.05,2), sqrt(e'*Omega*e)<=delta, here unused
    const float thHuber3D = sqrt(7.815);//chi2(0.05,3)

    // Set MapPoint vertices
    for(size_t i=0; i<vpMP.size(); i++)
    {
        MapPoint* pMP = vpMP[i];
        if(pMP->isBad())
            continue;
        g2o::VertexSBAPointXYZ* vPoint = new g2o::VertexSBAPointXYZ();
        vPoint->setEstimate(Converter::toVector3d(pMP->GetWorldPos()));
        const int id = pMP->mnId+maxKFid+1;//same as localBA
        vPoint->setId(id);
        vPoint->setMarginalized(true);//P(xc,xp)=P(xc)*P(xp|xc), P(xc) is called marginalized/Schur elimination, [B-E*C^(-1)*E.t()]*deltaXc=v-E*C^(-1)*w, H*deltaX=g=[v;w]; used in Sparse solver
        optimizer.addVertex(vPoint);

       const map<KeyFrame*,size_t> observations = pMP->GetObservations();

        int nEdges = 0;
        //SET EDGES
        for(map<KeyFrame*,size_t>::const_iterator mit=observations.begin(); mit!=observations.end(); mit++)
        {

            KeyFrame* pKF = mit->first;
            if(pKF->isBad() || pKF->mnId>maxKFid)//pKF->mnId may > maxKFid for LocalMapping is recovered
                continue;//only connect observation edges to optimized KFs/vertices

            nEdges++;

            const cv::KeyPoint &kpUn = pKF->mvKeysUn[mit->second];

            if(pKF->mvuRight[mit->second]<0)//monocular MPs use 2*1 error
            {
                Eigen::Matrix<double,2,1> obs;
                obs << kpUn.pt.x, kpUn.pt.y;

                g2o::EdgeSE3ProjectXYZ* e = new g2o::EdgeSE3ProjectXYZ();

                e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));//0 is point
                e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKF->mnId)));//1 is pose
                e->setMeasurement(obs);
                const float &invSigma2 = pKF->mvInvLevelSigma2[kpUn.octave];//1/sigma^2
                e->setInformation(Eigen::Matrix2d::Identity()*invSigma2);//Omega=Sigma^(-1)=(here)=I/sigma^2

                if(bRobust)//here false
                {
                    g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuber2D);
                }

                e->fx = pKF->fx;
                e->fy = pKF->fy;
                e->cx = pKF->cx;
                e->cy = pKF->cy;

                optimizer.addEdge(e);
            }
            else//stereo MPs uses 3*1 error
            {
                Eigen::Matrix<double,3,1> obs;
                const float kp_ur = pKF->mvuRight[mit->second];
                obs << kpUn.pt.x, kpUn.pt.y, kp_ur;

                g2o::EdgeStereoSE3ProjectXYZ* e = new g2o::EdgeStereoSE3ProjectXYZ();

                e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
                e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKF->mnId)));
                e->setMeasurement(obs);
                const float &invSigma2 = pKF->mvInvLevelSigma2[kpUn.octave];
                Eigen::Matrix3d Info = Eigen::Matrix3d::Identity()*invSigma2;
                e->setInformation(Info);

                if(bRobust)
                {
                    g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuber3D);
                }

                //set camera intrinsics(including b*f) to the edge
                e->fx = pKF->fx;
                e->fy = pKF->fy;
                e->cx = pKF->cx;
                e->cy = pKF->cy;
                e->bf = pKF->mbf;

                optimizer.addEdge(e);
            }
        }

        if(nEdges==0)//if no edges/observations can be made, delete this Vertex from optimizer
        {
            optimizer.removeVertex(vPoint);//here only do _vertices.erase(it(it->second==vPoint))
            vbNotIncludedMP[i]=true;
        }
        else
        {
            vbNotIncludedMP[i]=false;
        }
    }

    // Optimize!
    optimizer.initializeOptimization();
    optimizer.optimize(nIterations);//10 same as pure inliers iterations in localBA/motion-only BA/Sim3Motion-only BA, maybe stopped by next CorrectLoop() in LoopClosing

    // Recover optimized data in a intermediate way

    //Keyframes
    for(size_t i=0; i<vpKFs.size(); i++)//all KFs in mpMap
    {
        KeyFrame* pKF = vpKFs[i];
        if(pKF->isBad())
            continue;
        g2o::VertexSE3Expmap* vSE3 = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(pKF->mnId));
        g2o::SE3Quat SE3quat = vSE3->estimate();
        if(nLoopKF==0)//I think this impossible for mpCurrentKF in LoopClosing cannot be id0 KF
        {
            pKF->SetPose(Converter::toCvMat(SE3quat));
        }
        else
        {
            pKF->mTcwGBA.create(4,4,CV_32F);
            Converter::toCvMat(SE3quat).copyTo(pKF->mTcwGBA);
            pKF->mnBAGlobalForKF = nLoopKF;
        }
    }

    //Points
    for(size_t i=0; i<vpMP.size(); i++)//all MPs in mpMap
    {
        if(vbNotIncludedMP[i])
            continue;
	//if this MP is optimized by g2o

        MapPoint* pMP = vpMP[i];

        if(pMP->isBad())
            continue;
        g2o::VertexSBAPointXYZ* vPoint = static_cast<g2o::VertexSBAPointXYZ*>(optimizer.vertex(pMP->mnId+maxKFid+1));

        if(nLoopKF==0)//I think it's impossible
        {
            pMP->SetWorldPos(Converter::toCvMat(vPoint->estimate()));
            pMP->UpdateNormalAndDepth();
        }
        else
        {
            pMP->mPosGBA.create(3,1,CV_32F);
            Converter::toCvMat(vPoint->estimate()).copyTo(pMP->mPosGBA);
            pMP->mnBAGlobalForKF = nLoopKF;
        }
    }

}

int Optimizer::PoseOptimization(Frame *pFrame)
{
    g2o::SparseOptimizer optimizer;
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;//6*1 is Li algebra/se3, 3*1 is location of landmark, here only use unary edge of 6, 3 won't be optimized=>motion(6*1)-only BA

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();//linearSolver<MatrixType> use dense solver

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);//BlockSolver covers linearSolver

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);//descending/optimization strategy is LM; this (BlockSolver*)_solver will be deleted in ~BaseClass()
    optimizer.setAlgorithm(solver);//delete solver/_algorithm in ~SparseOptimizer()

    int nInitialCorrespondences=0;

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();//6*1 vertex
    vSE3->setEstimate(Converter::toSE3Quat(pFrame->mTcw));//here g2o vertex uses Tcw(different in default VertexSE3 using EdgeSE3); edge/measurement formula input ξ
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set MapPoint vertices
    const int N = pFrame->N;

    vector<g2o::EdgeSE3ProjectXYZOnlyPose*> vpEdgesMono;//2*1(_measurement) unary edge<VertexSE3Expmap>
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);//this can be optimized in RGBD mode

    vector<g2o::EdgeStereoSE3ProjectXYZOnlyPose*> vpEdgesStereo;//3*1(ul vl ur) unary edge
    vector<size_t> vnIndexEdgeStereo;
    vpEdgesStereo.reserve(N);
    vnIndexEdgeStereo.reserve(N);

    const float deltaMono = sqrt(5.991);//chi2(0.05,2)
    const float deltaStereo = sqrt(7.815);//chi2 distribution chi2(0.05,3), the huber kernel delta


    {
    unique_lock<mutex> lock(MapPoint::mGlobalMutex);//forbid other threads to rectify pFrame->mvpMapPoints

    for(int i=0; i<N; i++)
    {
        MapPoint* pMP = pFrame->mvpMapPoints[i];
        if(pMP)
        {
            // Monocular observation
            if(pFrame->mvuRight[i]<0)//this may happen in RGBD case!
            {
                nInitialCorrespondences++;
                pFrame->mvbOutlier[i] = false;

                Eigen::Matrix<double,2,1> obs;
                const cv::KeyPoint &kpUn = pFrame->mvKeysUn[i];
                obs << kpUn.pt.x, kpUn.pt.y;

                g2o::EdgeSE3ProjectXYZOnlyPose* e = new g2o::EdgeSE3ProjectXYZOnlyPose();

                e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
                e->setMeasurement(obs);
                const float invSigma2 = pFrame->mvInvLevelSigma2[kpUn.octave];
                e->setInformation(Eigen::Matrix2d::Identity()*invSigma2);

                g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                e->setRobustKernel(rk);
                rk->setDelta(deltaMono);

                e->fx = pFrame->fx;
                e->fy = pFrame->fy;
                e->cx = pFrame->cx;
                e->cy = pFrame->cy;
                cv::Mat Xw = pMP->GetWorldPos();
                e->Xw[0] = Xw.at<float>(0);
                e->Xw[1] = Xw.at<float>(1);
                e->Xw[2] = Xw.at<float>(2);

                optimizer.addEdge(e);

                vpEdgesMono.push_back(e);
                vnIndexEdgeMono.push_back(i);
            }
            else  // Stereo observation
            {
                nInitialCorrespondences++;
                pFrame->mvbOutlier[i] = false;

                //SET EDGE
                Eigen::Matrix<double,3,1> obs;
                const cv::KeyPoint &kpUn = pFrame->mvKeysUn[i];
                const float &kp_ur = pFrame->mvuRight[i];
                obs << kpUn.pt.x, kpUn.pt.y, kp_ur;

                g2o::EdgeStereoSE3ProjectXYZOnlyPose* e = new g2o::EdgeStereoSE3ProjectXYZOnlyPose();

                e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));//this dynamic_cast is useless
                e->setMeasurement(obs);//edge parameter/measurement formula output z
                const float invSigma2 = pFrame->mvInvLevelSigma2[kpUn.octave];
                Eigen::Matrix3d Info = Eigen::Matrix3d::Identity()*invSigma2;//optimization target block=|e'*Omiga(or Sigma^(-1))*e|, diagonal matrix means independece between x and y pixel noise
                e->setInformation(Info);//3*3 matrix

                g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;//optimization target=KernelHuber(block)=H(e)={1/2*e sqrt(e)<=delta;delta(sqrt(e)-1/2*delta) others}
                e->setRobustKernel(rk);
                rk->setDelta(deltaStereo);

                e->fx = pFrame->fx;
                e->fy = pFrame->fy;
                e->cx = pFrame->cx;
                e->cy = pFrame->cy;
                e->bf = pFrame->mbf;
                cv::Mat Xw = pMP->GetWorldPos();//edge/measurement formula parameter Xw
                e->Xw[0] = Xw.at<float>(0);
                e->Xw[1] = Xw.at<float>(1);
                e->Xw[2] = Xw.at<float>(2);

                optimizer.addEdge(e);//_error is the edge output

                vpEdgesStereo.push_back(e);
                vnIndexEdgeStereo.push_back(i);//record the edge recording feature index
            }
        }

    }
    }


    if(nInitialCorrespondences<3)//at least P3P（well posed equation） EPnP(n>3) (overdetermined equation)
        return 0;

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={5.991,5.991,5.991,5.991};
    const float chi2Stereo[4]={7.815,7.815,7.815, 7.815};//chi2(0.05,3), error_block limit(over will be outliers,here also lead to turning point in RobustKernelHuber)
    const int its[4]={10,10,10,10};    

    int nBad=0;
    for(size_t it=0; it<4; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(pFrame->mTcw));//4 optimizations, each 10 steps, initial value is the same, but inliers are different
        optimizer.initializeOptimization(0);//default edges' level is 0, so initially use all edges to optimize, after it=0, just use inlier edges(_activeEdges) to optimize
        optimizer.optimize(its[it]);//only call _activeEdges[k].computeError()

        nBad=0;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)//for 3D-monocular 2D matches, may entered in RGBD!
        {
            g2o::EdgeSE3ProjectXYZOnlyPose* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(pFrame->mvbOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
            {                
                pFrame->mvbOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                pFrame->mvbOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }

        for(size_t i=0, iend=vpEdgesStereo.size(); i<iend; i++)//for 3D-stereo 2D matches
        {
            g2o::EdgeStereoSE3ProjectXYZOnlyPose* e = vpEdgesStereo[i];

            const size_t idx = vnIndexEdgeStereo[i];

            if(pFrame->mvbOutlier[idx])//at 1st time, all false for all edges is at level 0 or inliers(supposed),so e._error is computed by g2o
            {
                e->computeError();
            }

            const float chi2 = e->chi2();//chi2=e'*Omiga*e

            if(chi2>chi2Stereo[it])
            {
                pFrame->mvbOutlier[idx]=true;
                e->setLevel(1);//adjust the outlier edges' level to 1
                nBad++;
            }
            else
            {                
                e->setLevel(0);//maybe adjust the outliers' level to inliers' level
                pFrame->mvbOutlier[idx]=false;
            }

            if(it==2)
                e->setRobustKernel(0);//let the final(it==3) optimization use no RobustKernel; this function will delete _robustkernel first
        }

        if(optimizer.edges().size()<10)//it outliers+inliers(/_edges) number<10 only optimize once with RobustKernelHuber
            break;
    }    

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);
    pFrame->SetPose(pose);//update posematrices of pFrame

    return nInitialCorrespondences-nBad;//number of inliers
}

void Optimizer::LocalBundleAdjustment(KeyFrame *pKF, bool* pbStopFlag, Map* pMap)
{    
    // Local KeyFrames: First Breath Search from Current Keyframe
    list<KeyFrame*> lLocalKeyFrames;

    lLocalKeyFrames.push_back(pKF);
    pKF->mnBALocalForKF = pKF->mnId;

    //insert all 1st layer Covisibility KFs into lLocalKeyFrames(not best n)
    const vector<KeyFrame*> vNeighKFs = pKF->GetVectorCovisibleKeyFrames();
    for(int i=0, iend=vNeighKFs.size(); i<iend; i++)
    {
        KeyFrame* pKFi = vNeighKFs[i];
        pKFi->mnBALocalForKF = pKF->mnId;
        if(!pKFi->isBad())
            lLocalKeyFrames.push_back(pKFi);//no covisible KFs(mvpOrderedConnectedKeyFrames) are duplicated or pKF itself
    }

    // Local MapPoints seen in Local KeyFrames
    list<MapPoint*> lLocalMapPoints;
    for(list<KeyFrame*>::iterator lit=lLocalKeyFrames.begin() , lend=lLocalKeyFrames.end(); lit!=lend; lit++)
    {
        vector<MapPoint*> vpMPs = (*lit)->GetMapPointMatches();
        for(vector<MapPoint*>::iterator vit=vpMPs.begin(), vend=vpMPs.end(); vit!=vend; vit++)
        {
            MapPoint* pMP = *vit;
            if(pMP)
                if(!pMP->isBad())
                    if(pMP->mnBALocalForKF!=pKF->mnId)//avoid duplications
                    {
                        lLocalMapPoints.push_back(pMP);
                        pMP->mnBALocalForKF=pKF->mnId;
                    }
        }
    }

    // Fixed Keyframes. Keyframes that see Local MapPoints but that are not Local Keyframes, 2nd layer fixed neighbors(don't optimize them but contribute to the target min funcation)
    list<KeyFrame*> lFixedCameras;
    for(list<MapPoint*>::iterator lit=lLocalMapPoints.begin(), lend=lLocalMapPoints.end(); lit!=lend; lit++)
    {
        map<KeyFrame*,size_t> observations = (*lit)->GetObservations();
        for(map<KeyFrame*,size_t>::iterator mit=observations.begin(), mend=observations.end(); mit!=mend; mit++)
        {
            KeyFrame* pKFi = mit->first;

            if(pKFi->mnBALocalForKF!=pKF->mnId && pKFi->mnBAFixedForKF!=pKF->mnId)//avoid duplications in lLocalKeyFrames && lFixedCameras
            {                
                pKFi->mnBAFixedForKF=pKF->mnId;
                if(!pKFi->isBad())
                    lFixedCameras.push_back(pKFi);
            }
        }
    }

    // Setup optimizer
    g2o::SparseOptimizer optimizer;
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;//<6,3> at least one type of BaseVertex<6/3,>

    linearSolver = new g2o::LinearSolverEigen<g2o::BlockSolver_6_3::PoseMatrixType>();//sparse Cholesky solver, similar to CSparse

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);//LM descending method
    optimizer.setAlgorithm(solver);

    if(pbStopFlag)//if &mbAbortBA !=nullptr, true in LocalMapping
        optimizer.setForceStopFlag(pbStopFlag);

    unsigned long maxKFid = 0;

    // Set Local KeyFrame vertices
    for(list<KeyFrame*>::iterator lit=lLocalKeyFrames.begin(), lend=lLocalKeyFrames.end(); lit!=lend; lit++)
    {
        KeyFrame* pKFi = *lit;
        g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();//<6,SE3Quat> vertex, 6 means the double update[6], SE3Quat means _estimate, for KFs' Tcw
        vSE3->setEstimate(Converter::toSE3Quat(pKFi->GetPose()));//set initial vertex value(SE3Quat(Tcw),meaning Tcw(SE3) but save like ξ(se3))
        vSE3->setId(pKFi->mnId);//not the order in lLocalKeyFrames
        vSE3->setFixed(pKFi->mnId==0);//only fix the vertex of initial KF(KF.mnId==0)
        optimizer.addVertex(vSE3);
        if(pKFi->mnId>maxKFid)
            maxKFid=pKFi->mnId;
    }

    // Set Fixed KeyFrame vertices
    for(list<KeyFrame*>::iterator lit=lFixedCameras.begin(), lend=lFixedCameras.end(); lit!=lend; lit++)
    {
        KeyFrame* pKFi = *lit;
        g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
        vSE3->setEstimate(Converter::toSE3Quat(pKFi->GetPose()));
        vSE3->setId(pKFi->mnId);
        vSE3->setFixed(true);//notice here for not optimizing these Tcw or they only contribute to the target function/e_block
        optimizer.addVertex(vSE3);
        if(pKFi->mnId>maxKFid)
            maxKFid=pKFi->mnId;
    }

    // Set MapPoint vertices && MPs-KFs' edges
    const int nExpectedSize = (lLocalKeyFrames.size()+lFixedCameras.size())*lLocalMapPoints.size();//max edges' size

    vector<g2o::EdgeSE3ProjectXYZ*> vpEdgesMono;//<2,Vector2d,VertexSBAPointXYZ,VertexSE3Expmap>; 2 means _error is Vector2d, Vector2d means _measurement;_vertices[0] is VertexSBAPointXYZ, _vertices[1] is VertexSE3Expmap
    vpEdgesMono.reserve(nExpectedSize);

    vector<KeyFrame*> vpEdgeKFMono;
    vpEdgeKFMono.reserve(nExpectedSize);

    vector<MapPoint*> vpMapPointEdgeMono;
    vpMapPointEdgeMono.reserve(nExpectedSize);

    vector<g2o::EdgeStereoSE3ProjectXYZ*> vpEdgesStereo;//<3,Vector3d,VertexSBAPointXYZ,VertexSE3Expmap>; _measurement is [ul;v;ur]
    vpEdgesStereo.reserve(nExpectedSize);

    vector<KeyFrame*> vpEdgeKFStereo;
    vpEdgeKFStereo.reserve(nExpectedSize);

    vector<MapPoint*> vpMapPointEdgeStereo;
    vpMapPointEdgeStereo.reserve(nExpectedSize);

    const float thHuberMono = sqrt(5.991);//sqrt(e_block)<=sqrt(chi2(0.05,2)) allow power 2 increasing(1/2*e_block), e_block=e'*Omiga*e like e_square
    const float thHuberStereo = sqrt(7.815);//chi2(0.05,3)

    for(list<MapPoint*>::iterator lit=lLocalMapPoints.begin(), lend=lLocalMapPoints.end(); lit!=lend; lit++)
    {
	//Set MP vertices
        MapPoint* pMP = *lit;
        g2o::VertexSBAPointXYZ* vPoint = new g2o::VertexSBAPointXYZ();//<3,Eigen::Vector3d>, for MPs' Xw
        vPoint->setEstimate(Converter::toVector3d(pMP->GetWorldPos()));
        int id = pMP->mnId+maxKFid+1;//>=maxKFid+1
        vPoint->setId(id);
        vPoint->setMarginalized(true);//P(xc,xp)=P(xc)*P(xp|xc), P(xc) is called marginalized/Schur elimination, [B-E*C^(-1)*E.t()]*deltaXc=v-E*C^(-1)*w, H*deltaX=g=[v;w]; used in Sparse solver
        optimizer.addVertex(vPoint);

        const map<KeyFrame*,size_t> observations = pMP->GetObservations();

        //Set edges
        for(map<KeyFrame*,size_t>::const_iterator mit=observations.begin(), mend=observations.end(); mit!=mend; mit++)
        {
            KeyFrame* pKFi = mit->first;

            if(!pKFi->isBad())//good pKFobserv then connect it with pMP by an edge
            {                
                const cv::KeyPoint &kpUn = pKFi->mvKeysUn[mit->second];

                // Monocular observation
                if(pKFi->mvuRight[mit->second]<0)
                {
                    Eigen::Matrix<double,2,1> obs;
                    obs << kpUn.pt.x, kpUn.pt.y;

                    g2o::EdgeSE3ProjectXYZ* e = new g2o::EdgeSE3ProjectXYZ();

                    e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));//0 VertexSBAPointXYZ* corresponding to pMP->mWorldPos
                    e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKFi->mnId)));//1 VertexSE3Expmap* corresponding to pKF->Tcw
                    e->setMeasurement(obs);
                    const float &invSigma2 = pKFi->mvInvLevelSigma2[kpUn.octave];
                    e->setInformation(Eigen::Matrix2d::Identity()*invSigma2);//Omiga=Sigma^(-1), here 2*2 for e_block=e'*Omiga*e

                    g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuberMono);//similar to ||e||

                    e->fx = pKFi->fx;
                    e->fy = pKFi->fy;
                    e->cx = pKFi->cx;
                    e->cy = pKFi->cy;

                    optimizer.addEdge(e);
                    vpEdgesMono.push_back(e);
                    vpEdgeKFMono.push_back(pKFi);//_vertices[1]
                    vpMapPointEdgeMono.push_back(pMP);//_vertices[0]
                }
                else // Stereo observation
                {
                    Eigen::Matrix<double,3,1> obs;
                    const float kp_ur = pKFi->mvuRight[mit->second];
                    obs << kpUn.pt.x, kpUn.pt.y, kp_ur;

                    g2o::EdgeStereoSE3ProjectXYZ* e = new g2o::EdgeStereoSE3ProjectXYZ();

                    e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
                    e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKFi->mnId)));
                    e->setMeasurement(obs);
                    const float &invSigma2 = pKFi->mvInvLevelSigma2[kpUn.octave];
                    Eigen::Matrix3d Info = Eigen::Matrix3d::Identity()*invSigma2;//3*3, notice use Omega to make ||e||/sqrt(e'*Omega*e) has sigma=1, can directly use chi2 standard distribution
                    e->setInformation(Info);

                    g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuberStereo);

                    e->fx = pKFi->fx;
                    e->fy = pKFi->fy;
                    e->cx = pKFi->cx;
                    e->cy = pKFi->cy;
                    e->bf = pKFi->mbf;//parameter addition b(m)*f

                    optimizer.addEdge(e);
                    vpEdgesStereo.push_back(e);
                    vpEdgeKFStereo.push_back(pKFi);
                    vpMapPointEdgeStereo.push_back(pMP);
                }
            }
        }
    }

    if(pbStopFlag)//true in LocalMapping
        if(*pbStopFlag)//if mbAbortBA
            return;

    optimizer.initializeOptimization();
    optimizer.optimize(5);//maybe stopped by *_forceStopFlag(mbAbortBA) in some step/iteration

    bool bDoMore= true;

    if(pbStopFlag)
        if(*pbStopFlag)//judge mbAbortBA again
            bDoMore = false;

    if(bDoMore)
    {

      // Check inlier observations
      for(size_t i=0, iend=vpEdgesMono.size(); i<iend;i++)
      {
	  g2o::EdgeSE3ProjectXYZ* e = vpEdgesMono[i];
	  MapPoint* pMP = vpMapPointEdgeMono[i];

	  if(pMP->isBad())//why this can be true?
	      continue;

	  if(e->chi2()>5.991 || !e->isDepthPositive())//if chi2 error too big(5% wrong) or Zc<=0 then outlier
	  {
	      e->setLevel(1);
	  }

	  e->setRobustKernel(0);//cancel RobustKernel
      }

      for(size_t i=0, iend=vpEdgesStereo.size(); i<iend;i++)
      {
	  g2o::EdgeStereoSE3ProjectXYZ* e = vpEdgesStereo[i];
	  MapPoint* pMP = vpMapPointEdgeStereo[i];

	  if(pMP->isBad())
	      continue;

	  if(e->chi2()>7.815 || !e->isDepthPositive())//chi2(0.05,3)
	  {
	      e->setLevel(1);
	  }

	  e->setRobustKernel(0);
      }

      // Optimize again without the outliers

      optimizer.initializeOptimization(0);
      optimizer.optimize(10);//10 steps same as motion-only BA

    }

    vector<pair<KeyFrame*,MapPoint*> > vToErase;
    vToErase.reserve(vpEdgesMono.size()+vpEdgesStereo.size());

    // Check inlier observations       
    for(size_t i=0, iend=vpEdgesMono.size(); i<iend;i++)
    {
        g2o::EdgeSE3ProjectXYZ* e = vpEdgesMono[i];
        MapPoint* pMP = vpMapPointEdgeMono[i];

        if(pMP->isBad())
            continue;

        if(e->chi2()>5.991 || !e->isDepthPositive())
        {
            KeyFrame* pKFi = vpEdgeKFMono[i];
            vToErase.push_back(make_pair(pKFi,pMP));//ready to erase outliers of pKFi && pMP in monocular edges
        }
    }

    for(size_t i=0, iend=vpEdgesStereo.size(); i<iend;i++)
    {
        g2o::EdgeStereoSE3ProjectXYZ* e = vpEdgesStereo[i];
        MapPoint* pMP = vpMapPointEdgeStereo[i];

        if(pMP->isBad())
            continue;

        if(e->chi2()>7.815 || !e->isDepthPositive())
        {
            KeyFrame* pKFi = vpEdgeKFStereo[i];
            vToErase.push_back(make_pair(pKFi,pMP));//ready to erase outliers of pKFi && pMP in stereo edges
        }
    }

    // Get Map Mutex
    unique_lock<mutex> lock(pMap->mMutexMapUpdate);

    if(!vToErase.empty())//erase the relation between outliers of pKFi(matched mvpMapPoints) && pMP(mObservations)
    {
        for(size_t i=0;i<vToErase.size();i++)
        {
            KeyFrame* pKFi = vToErase[i].first;
            MapPoint* pMPi = vToErase[i].second;
            pKFi->EraseMapPointMatch(pMPi);
            pMPi->EraseObservation(pKFi);//here may erase pMP in mpMap
        }
    }

    // Recover optimized data

    //Keyframes update(Pose Tcw...)
    for(list<KeyFrame*>::iterator lit=lLocalKeyFrames.begin(), lend=lLocalKeyFrames.end(); lit!=lend; lit++)
    {
        KeyFrame* pKF = *lit;
        g2o::VertexSE3Expmap* vSE3 = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(pKF->mnId));
        g2o::SE3Quat SE3quat = vSE3->estimate();
        pKF->SetPose(Converter::toCvMat(SE3quat));//pKF->SetPose(optimized Tcw)
    }

    //Points update(Position, normal), no need to update descriptor for unchanging pMP->mObservations except for the ones having outliers' edges?
    for(list<MapPoint*>::iterator lit=lLocalMapPoints.begin(), lend=lLocalMapPoints.end(); lit!=lend; lit++)
    {
        MapPoint* pMP = *lit;
        g2o::VertexSBAPointXYZ* vPoint = static_cast<g2o::VertexSBAPointXYZ*>(optimizer.vertex(pMP->mnId+maxKFid+1));
        pMP->SetWorldPos(Converter::toCvMat(vPoint->estimate()));
        pMP->UpdateNormalAndDepth();
    }
}


void Optimizer::OptimizeEssentialGraph(Map* pMap, KeyFrame* pLoopKF, KeyFrame* pCurKF,
                                       const LoopClosing::KeyFrameAndPose &NonCorrectedSim3,
                                       const LoopClosing::KeyFrameAndPose &CorrectedSim3,
                                       const map<KeyFrame *, set<KeyFrame *> > &LoopConnections, const bool &bFixScale)
{
    // Setup optimizer
    g2o::SparseOptimizer optimizer;
    //optimizer.setVerbose(false);//useless for initially _verbose(false)
    g2o::BlockSolver_7_3::LinearSolverType * linearSolver =
           new g2o::LinearSolverEigen<g2o::BlockSolver_7_3::PoseMatrixType>();//sparse Cholesky solver
    g2o::BlockSolver_7_3 * solver_ptr= new g2o::BlockSolver_7_3(linearSolver);//7 is sim3's dimension, 3 is position's dimension(unused here)
    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);//LM descending method

    solver->setUserLambdaInit(1e-16);//solver->_userLambdaInit->_value=0 at initial, here use 1e-16 for initial fast descending(near Steepest Descent instead of Gauss-Newton)
    optimizer.setAlgorithm(solver);

    const vector<KeyFrame*> vpKFs = pMap->GetAllKeyFrames();
    const vector<MapPoint*> vpMPs = pMap->GetAllMapPoints();

    const unsigned int nMaxKFid = pMap->GetMaxKFid();

    vector<g2o::Sim3,Eigen::aligned_allocator<g2o::Sim3> > vScw(nMaxKFid+1);//+1 for id is from 0, Eigen::aligned_allocator is for Eigen::Quaterniond in g2o::Sim3, vScw used mainly for all MPs' correction
    vector<g2o::Sim3,Eigen::aligned_allocator<g2o::Sim3> > vCorrectedSwc(nMaxKFid+1);
    //vector<g2o::VertexSim3Expmap*> vpVertices(nMaxKFid+1);//unused here

    const int minFeat = 100;//min loop edges' covisible MPs' number threshold

    // Set KeyFrame vertices
    for(size_t i=0, iend=vpKFs.size(); i<iend;i++)//all KFs in pMap
    {
        KeyFrame* pKF = vpKFs[i];
        if(pKF->isBad())
            continue;
        g2o::VertexSim3Expmap* VSim3 = new g2o::VertexSim3Expmap();

        const int nIDi = pKF->mnId;

        LoopClosing::KeyFrameAndPose::const_iterator it = CorrectedSim3.find(pKF);

        if(it!=CorrectedSim3.end())//if pKF is found in CorrectedSim3
        {
            vScw[nIDi] = it->second;
            VSim3->setEstimate(it->second);//corrected g2oScw, here c means camera
        }
        else
        {
            Eigen::Matrix<double,3,3> Rcw = Converter::toMatrix3d(pKF->GetRotation());
            Eigen::Matrix<double,3,1> tcw = Converter::toVector3d(pKF->GetTranslation());
            g2o::Sim3 Siw(Rcw,tcw,1.0);
            vScw[nIDi] = Siw;
            VSim3->setEstimate(Siw);//noncorrected g2oScw
        }

        if(pKF==pLoopKF)
            VSim3->setFixed(true);//fix mpMatchedKF(in LoopClosing), this means id0 KF's Pose will be optimized here, but it will be only changed a little for mpMatchedKF's Pose is relatively accurate!
	//maybe fix mpMatchedKF instead of id0 KF for g2o optimization is start from fixed Pose, so if time is limited, optimizing the close in time KFs first is better

        VSim3->setId(nIDi);
        //VSim3->setMarginalized(false);//useless for initially _marginalized(false)
        VSim3->_fix_scale = bFixScale;//true/s=1 in VertexSE3Expmap for RGBD

        optimizer.addVertex(VSim3);

        //vpVertices[nIDi]=VSim3;
    }


    set<pair<long unsigned int,long unsigned int> > sInsertedEdges;

    const Eigen::Matrix<double,7,7> matLambda = Eigen::Matrix<double,7,7>::Identity();//information matrix/Omega/Sigma^(-1) uses default or use Euclidean distance \
    instead of Mahalonobis in error_block=e'e instead of e'Omega(!=I)e, so we also don't use RobustKernel/chi2/outliers/scale concept here, \
    so maybe we can use erroneous edges' concept in RGBDSLAM2 here!

    // Set Loop edges
    for(map<KeyFrame *, set<KeyFrame *> >::const_iterator mit = LoopConnections.begin(), mend=LoopConnections.end(); mit!=mend; mit++)
    {
        KeyFrame* pKF = mit->first;
        const long unsigned int nIDi = pKF->mnId;
        const set<KeyFrame*> &spConnections = mit->second;
        const g2o::Sim3 Siw = vScw[nIDi];//not only faster than optimizer.vertex(nIDi)->estimate() but also used to correct MPs in the end of this function, corrected Siw
        const g2o::Sim3 Swi = Siw.inverse();//corrected Swi

        for(set<KeyFrame*>::const_iterator sit=spConnections.begin(), send=spConnections.end(); sit!=send; sit++)
        {
            const long unsigned int nIDj = (*sit)->mnId;
            if((nIDi!=pCurKF->mnId || nIDj!=pLoopKF->mnId) && pKF->GetWeight(*sit)<minFeat)
                continue;
	    //if i is pCurKF && j is pLoopKF or pKF,*sit's covisible MPs' number>=100 here(notice 15 can have covisible connections, \
	    meaning Essential graph is a smaller(closer) part of Covisibility graph, here use stricter condition for it's new loop edges(validation in PoseGraphBA))

            const g2o::Sim3 Sjw = vScw[nIDj];//noncorrected but relatively accurate Sjw
            const g2o::Sim3 Sji = Sjw * Swi;//relatively accurate Sji

            g2o::EdgeSim3* e = new g2o::EdgeSim3();//vertex 0/1 VertexSE3Expmap
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDj)));//1 j
            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDi)));//0 i
            e->setMeasurement(Sji);//S10/Sji~=Sjw*Siw^(-1), notice in default g2o lib g2o::EdgeSE3&&g2o::VertexSE3 use Tij&&Twi

            e->information() = matLambda;//error_block=e'e

            optimizer.addEdge(e);

            sInsertedEdges.insert(make_pair(min(nIDi,nIDj),max(nIDi,nIDj)));//use min&&max to avoid duplications like (i,j)&&(j,i) for loop edges(in Essential/Pose Graph) are undirected edges
        }
    }

    // Set normal edges
    for(size_t i=0, iend=vpKFs.size(); i<iend; i++)//all KFs in pMap
    {
        KeyFrame* pKF = vpKFs[i];

        const int nIDi = pKF->mnId;

        g2o::Sim3 Swi;

        LoopClosing::KeyFrameAndPose::const_iterator iti = NonCorrectedSim3.find(pKF);

        if(iti!=NonCorrectedSim3.end())//if pKF in found in NonCorrectedSim3/mvpCurrentConnectedKFs in LoopClosing
            Swi = (iti->second).inverse();//noncorrected Swi
        else
            Swi = vScw[nIDi].inverse();//noncorrected Swi

        KeyFrame* pParentKF = pKF->GetParent();

        // Spanning tree edge, like VO/visual odometry edges
        if(pParentKF)
        {
            int nIDj = pParentKF->mnId;

            g2o::Sim3 Sjw;

            LoopClosing::KeyFrameAndPose::const_iterator itj = NonCorrectedSim3.find(pParentKF);

	    //noncorrected Swj
            if(itj!=NonCorrectedSim3.end())
                Sjw = itj->second;
            else
                Sjw = vScw[nIDj];

            g2o::Sim3 Sji = Sjw * Swi;//get Sji before CorrectLoop() in LoopClosing; noncorrected: Sji=Sjw*Swi;

            g2o::EdgeSim3* e = new g2o::EdgeSim3();
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDj)));
            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDi)));
            e->setMeasurement(Sji);

            e->information() = matLambda;//I
            optimizer.addEdge(e);
        }

        // Loop edges, previous ones added by the order of el->vertex(0)->id() < el->vertex(1)->id()
        const set<KeyFrame*> sLoopEdges = pKF->GetLoopEdges();//loop edges before this CorrectLoop()/previous mspLoopEdges of pKF, \
	notice only one(pCurKF-pLoopKF) of new loop edges will be mspLoopEdges, the reason why PoseGraphBA need lots of new loop edges for a better believe on this loop close and \
	optimize all KFs' Poses basing on this loop close
        for(set<KeyFrame*>::const_iterator sit=sLoopEdges.begin(), send=sLoopEdges.end(); sit!=send; sit++)
        {
            KeyFrame* pLKF = *sit;
            if(pLKF->mnId<pKF->mnId)//avoid repetitively adding the same loop edge/ add loop edges in an ordered way
            {
                g2o::Sim3 Slw;

                LoopClosing::KeyFrameAndPose::const_iterator itl = NonCorrectedSim3.find(pLKF);

		//noncorrected(in this loop correction) Slw
                if(itl!=NonCorrectedSim3.end())
                    Slw = itl->second;
                else
                    Slw = vScw[pLKF->mnId];

                g2o::Sim3 Sli = Slw * Swi;//previous/noncorrected Sli
                g2o::EdgeSim3* el = new g2o::EdgeSim3();
                el->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pLKF->mnId)));
                el->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDi)));
                el->setMeasurement(Sli);
                el->information() = matLambda;//I
                optimizer.addEdge(el);
            }
        }

        // Covisibility graph edges, a smaller(closer) part whose weight>=100
        const vector<KeyFrame*> vpConnectedKFs = pKF->GetCovisiblesByWeight(minFeat);
        for(vector<KeyFrame*>::const_iterator vit=vpConnectedKFs.begin(); vit!=vpConnectedKFs.end(); vit++)
        {
            KeyFrame* pKFn = *vit;
            if(pKFn && pKFn!=pParentKF && !pKF->hasChild(pKFn) && !sLoopEdges.count(pKFn))//don't need to judge !pKFn->mspLoopEdges.count(pKF) for previous loop edges are symmetric, \
	      avoid duplications in optimizer._edges...;pKFn cannot be the parent/child of pKF(spanning tree edge) && previous loop edge of pKF
            {
                if(!pKFn->isBad() && pKFn->mnId<pKF->mnId)//good && avoid duplications by the order 1id<0id
                {
                    if(sInsertedEdges.count(make_pair(min(pKF->mnId,pKFn->mnId),max(pKF->mnId,pKFn->mnId))))//check if inserted as new loop edges before!
                        continue;

                    g2o::Sim3 Snw;//Snormal_world, normal means a smaller part of Covisibility graph but not the loop/spanning tree edges

                    LoopClosing::KeyFrameAndPose::const_iterator itn = NonCorrectedSim3.find(pKFn);

		    //noncorrected Snw
                    if(itn!=NonCorrectedSim3.end())
                        Snw = itn->second;
                    else
                        Snw = vScw[pKFn->mnId];

                    g2o::Sim3 Sni = Snw * Swi;//previous/noncorrected Sni

                    g2o::EdgeSim3* en = new g2o::EdgeSim3();
                    en->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKFn->mnId)));
                    en->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDi)));
                    en->setMeasurement(Sni);
                    en->information() = matLambda;//I
                    optimizer.addEdge(en);
                }
            }
        }
    }

    // Optimize!
    optimizer.initializeOptimization();//optimize all KFs' Pose Siw by new loop edges and normal edges
    optimizer.optimize(20);//2*10 steps, 10 is the pure inliers' iterations in localBA/motion-only BA/Sim3Motion-only BA

    unique_lock<mutex> lock(pMap->mMutexMapUpdate);

    // SE3 Pose Recovering. Sim3:[sR t;0 1] -> SE3:[R t/s;0 1]
    for(size_t i=0;i<vpKFs.size();i++)//all KFs in pMap
    {
        KeyFrame* pKFi = vpKFs[i];

        const int nIDi = pKFi->mnId;

        g2o::VertexSim3Expmap* VSim3 = static_cast<g2o::VertexSim3Expmap*>(optimizer.vertex(nIDi));
        g2o::Sim3 CorrectedSiw =  VSim3->estimate();//optimized Siw
        vCorrectedSwc[nIDi]=CorrectedSiw.inverse();//used for latter points' correction
        Eigen::Matrix3d eigR = CorrectedSiw.rotation().toRotationMatrix();
        Eigen::Vector3d eigt = CorrectedSiw.translation();
        double s = CorrectedSiw.scale();

        eigt *=(1./s); //[R t/s;0 1]

        cv::Mat Tiw = Converter::toCvSE3(eigR,eigt);

        pKFi->SetPose(Tiw);//update KF's Pose to PoseGraphBA optimized Tiw
    }

    // Correct points. Transform to "non-optimized" reference keyframe pose and transform back with optimized pose
    for(size_t i=0, iend=vpMPs.size(); i<iend; i++)//all MPs in pMap
    {
        MapPoint* pMP = vpMPs[i];

        if(pMP->isBad())
            continue;

        int nIDr;
        if(pMP->mnCorrectedByKF==pCurKF->mnId)//if this MP has already been correted by pCurKF/mpCurrentKF
        {
            nIDr = pMP->mnCorrectedReference;//vScw[nIDr(here)] is corrected Scw for mvpCurrentConnectedKFs, corresponding pMP's Pos is also corrected in CorrectLoop()
        }
        else//if this MP's Pos is noncorrected
        {
            KeyFrame* pRefKF = pMP->GetReferenceKeyFrame();
            nIDr = pRefKF->mnId;//vScw[nIDr] should be noncorrected one!
        }


        g2o::Sim3 Srw = vScw[nIDr];//corrected/noncorrected Scw but always can be used to calculate Pr=Scw*Pw
        g2o::Sim3 correctedSwr = vCorrectedSwc[nIDr];//BA optimized result

        cv::Mat P3Dw = pMP->GetWorldPos();
        Eigen::Matrix<double,3,1> eigP3Dw = Converter::toVector3d(P3Dw);
        Eigen::Matrix<double,3,1> eigCorrectedP3Dw = correctedSwr.map(Srw.map(eigP3Dw));//optimized Pw=optimized Swr*Pr(noncorrected Srw*noncorrected Pw/corrected Srw*corrected Pw)

        cv::Mat cvCorrectedP3Dw = Converter::toCvMat(eigCorrectedP3Dw);
        pMP->SetWorldPos(cvCorrectedP3Dw);//update MP's Pos to correted one through BA optimized Siw

        pMP->UpdateNormalAndDepth();//update MP's normal for its position changed
    }
}

int Optimizer::OptimizeSim3(KeyFrame *pKF1, KeyFrame *pKF2, vector<MapPoint *> &vpMatches1, g2o::Sim3 &g2oS12, const float th2, const bool bFixScale)
{
    g2o::SparseOptimizer optimizer;
    g2o::BlockSolverX::LinearSolverType * linearSolver;//<Eigen::Dynamic,Dynamic>

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolverX::PoseMatrixType>();//LinearSolver<MatrixType> uses a dense solver, the same as PoseOptimization()

    g2o::BlockSolverX * solver_ptr = new g2o::BlockSolverX(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);//LM descending method
    optimizer.setAlgorithm(solver);

    // Calibration
    const cv::Mat &K1 = pKF1->mK;
    const cv::Mat &K2 = pKF2->mK;

    // Camera poses
    const cv::Mat R1w = pKF1->GetRotation();
    const cv::Mat t1w = pKF1->GetTranslation();
    const cv::Mat R2w = pKF2->GetRotation();
    const cv::Mat t2w = pKF2->GetTranslation();

    // Set Sim3 vertex
    g2o::VertexSim3Expmap * vSim3 = new g2o::VertexSim3Expmap();    
    vSim3->_fix_scale=bFixScale;
    vSim3->setEstimate(g2oS12);//this vertex showing the relative Sim3 transform/g2o: S12/ScurrentKF_maploopcandidateKF(enough consistent)
    vSim3->setId(0);//same as PoseOptimization()
    vSim3->setFixed(false);//S12 to be optimized
    vSim3->_principle_point1[0] = K1.at<float>(0,2);//cx1
    vSim3->_principle_point1[1] = K1.at<float>(1,2);//cy1
    vSim3->_focal_length1[0] = K1.at<float>(0,0);//fx1
    vSim3->_focal_length1[1] = K1.at<float>(1,1);//fy1
    vSim3->_principle_point2[0] = K2.at<float>(0,2);//cx2
    vSim3->_principle_point2[1] = K2.at<float>(1,2);//cy2
    vSim3->_focal_length2[0] = K2.at<float>(0,0);//fx2
    vSim3->_focal_length2[1] = K2.at<float>(1,1);//fy2
    optimizer.addVertex(vSim3);

    // Set MapPoint vertices
    const int N = vpMatches1.size();
    const vector<MapPoint*> vpMapPoints1 = pKF1->GetMapPointMatches();
    vector<g2o::EdgeSim3ProjectXYZ*> vpEdges12;
    vector<g2o::EdgeInverseSim3ProjectXYZ*> vpEdges21;
    vector<size_t> vnIndexEdge;

    vnIndexEdge.reserve(2*N);
    vpEdges12.reserve(2*N);
    vpEdges21.reserve(2*N);

    const float deltaHuber = sqrt(th2);//here sqrt(10), upper bound/supremum of ||e||

    int nCorrespondences = 0;

    for(int i=0; i<N; i++)
    {
        if(!vpMatches1[i])
            continue;

        MapPoint* pMP1 = vpMapPoints1[i];
        MapPoint* pMP2 = vpMatches1[i];//pMP1's matched MP

        const int id1 = 2*i+1;//different from PoseOptimization()
        const int id2 = 2*(i+1);

        const int i2 = pMP2->GetIndexInKeyFrame(pKF2);

        if(pMP1 && pMP2)//I think it's always true for the SBP/SBB method in LoopClosing, need test!
        {
            if(!pMP1->isBad() && !pMP2->isBad() && i2>=0)//I this it's always true for SBP/SBB method in LoopClosing, need test! if wrong, it may be due to MapPointCulling() in LocalMapping
            {
                g2o::VertexSBAPointXYZ* vPoint1 = new g2o::VertexSBAPointXYZ();
                cv::Mat P3D1w = pMP1->GetWorldPos();
                cv::Mat P3D1c = R1w*P3D1w + t1w;
                vPoint1->setEstimate(Converter::toVector3d(P3D1c));
                vPoint1->setId(id1);
                vPoint1->setFixed(true);//don't optimize the position of matched MapPoints(pKF1,pKF2), just contribute to the target function
                optimizer.addVertex(vPoint1);

                g2o::VertexSBAPointXYZ* vPoint2 = new g2o::VertexSBAPointXYZ();
                cv::Mat P3D2w = pMP2->GetWorldPos();
                cv::Mat P3D2c = R2w*P3D2w + t2w;
                vPoint2->setEstimate(Converter::toVector3d(P3D2c));
                vPoint2->setId(id2);
                vPoint2->setFixed(true);
                optimizer.addVertex(vPoint2);
            }
            else
                continue;
        }
        else
            continue;

        nCorrespondences++;//the number of matches

        // Set edge x1 = S12*X2
        Eigen::Matrix<double,2,1> obs1;
        const cv::KeyPoint &kpUn1 = pKF1->mvKeysUn[i];
        obs1 << kpUn1.pt.x, kpUn1.pt.y;

        g2o::EdgeSim3ProjectXYZ* e12 = new g2o::EdgeSim3ProjectXYZ();//_vertices[0] VertexSBAPointXYZ; 1 VertexSim3Expmap
        e12->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id2)));//0 Xc2
        e12->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));//1 S12
        e12->setMeasurement(obs1);//[u1;v1]
        const float &invSigmaSquare1 = pKF1->mvInvLevelSigma2[kpUn1.octave];
        e12->setInformation(Eigen::Matrix2d::Identity()*invSigmaSquare1);//Omega/Sigma^(-1)

        g2o::RobustKernelHuber* rk1 = new g2o::RobustKernelHuber;
        e12->setRobustKernel(rk1);
        rk1->setDelta(deltaHuber);//here ||e||<=sqrt(10) use 1/2*e'Omega*e
        optimizer.addEdge(e12);//edge_S12_Xc2

        // Set edge x2 = S21*X1
        Eigen::Matrix<double,2,1> obs2;
        const cv::KeyPoint &kpUn2 = pKF2->mvKeysUn[i2];
        obs2 << kpUn2.pt.x, kpUn2.pt.y;

        g2o::EdgeInverseSim3ProjectXYZ* e21 = new g2o::EdgeInverseSim3ProjectXYZ();

        e21->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id1)));//0 Xc1
        e21->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));//1 S12/S21
        e21->setMeasurement(obs2);//[u2;v2]
        float invSigmaSquare2 = pKF2->mvInvLevelSigma2[kpUn2.octave];
        e21->setInformation(Eigen::Matrix2d::Identity()*invSigmaSquare2);//Omega

        g2o::RobustKernelHuber* rk2 = new g2o::RobustKernelHuber;
        e21->setRobustKernel(rk2);
        rk2->setDelta(deltaHuber);
        optimizer.addEdge(e21);//edge_S12/S21_Xc1

        vpEdges12.push_back(e12);
        vpEdges21.push_back(e21);
        vnIndexEdge.push_back(i);//i in pKF1, matched one is vpMatches1[i]->GetIndexInKeyFrame(pKF2) in pKF2
    }

    // Optimize!
    optimizer.initializeOptimization();
    optimizer.optimize(5);//same as LocalBundleAdjustment()

    // Check inliers
    int nBad=0;
    for(size_t i=0; i<vpEdges12.size();i++)
    {
        g2o::EdgeSim3ProjectXYZ* e12 = vpEdges12[i];
        g2o::EdgeInverseSim3ProjectXYZ* e21 = vpEdges21[i];
        if(!e12 || !e21)//I think this cannot be true, need test!
            continue;

        if(e12->chi2()>th2 || e21->chi2()>th2)
        {
            size_t idx = vnIndexEdge[i];
            vpMatches1[idx]=static_cast<MapPoint*>(NULL);//erase outlier matches immediately
            optimizer.removeEdge(e12);//erase e12 from the HyperGraph::EdgeSet _edges && HyperGraph::Vertex::EdgeSet _edges
            optimizer.removeEdge(e21);
            vpEdges12[i]=static_cast<g2o::EdgeSim3ProjectXYZ*>(NULL);
            vpEdges21[i]=static_cast<g2o::EdgeInverseSim3ProjectXYZ*>(NULL);
            nBad++;//the number of outliers
        }//about 99% inliers are right, chi2(0.01,2)=9.21, looser then localBA
    }

    int nMoreIterations;
    if(nBad>0)//if any outlier is found
        nMoreIterations=10;//same as localBA/motion-only BA
    else
        nMoreIterations=5;//5+5=10, always 10 iterations including only inliers

    if(nCorrespondences-nBad<10)//if the number of inliers <10 directly regard it's an unbelievable loop candidate match, \
      half of the used threshold in ComputeSim3() in LoopClosing, same as nGood<10 then continue threshold in Relocalization()
        return 0;

    // Optimize again only with inliers
    optimizer.initializeOptimization();
    optimizer.optimize(nMoreIterations);

    int nIn = 0;//the number of inliers
    for(size_t i=0; i<vpEdges12.size();i++)
    {
        g2o::EdgeSim3ProjectXYZ* e12 = vpEdges12[i];
        g2o::EdgeInverseSim3ProjectXYZ* e21 = vpEdges21[i];
        if(!e12 || !e21)//here maybe true for outliers checked before
            continue;

        if(e12->chi2()>th2 || e21->chi2()>th2)//if outliers
        {
            //size_t idx = vnIndexEdge[i];
            vpMatches1[vnIndexEdge[i]]=static_cast<MapPoint*>(NULL);//erase outlier matches immediately, rectified by zzh, here vnIndexEdge[i]==i if vpMatches1[i] is always two good MPs' match
        }
        else
            nIn++;
    }

    // Recover optimized Sim3
    g2o::VertexSim3Expmap* vSim3_recov = static_cast<g2o::VertexSim3Expmap*>(optimizer.vertex(0));//get optimized S12
    g2oS12= vSim3_recov->estimate();//rectify g2o: S12 to the optimized S12

    return nIn;
}


} //namespace ORB_SLAM
