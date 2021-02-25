/*
 * Software License Agreement (BSD License)
 *
 * Point Cloud Library (PCL) - www.pointclouds.org
 * Copyright (c) 2009-2012, Willow Garage, Inc.
 * Copyright (c) 2012-, Open Perception, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *  * Neither the name of the copyright holder(s) nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * $Id$
 *
 */

#include <pcl/apps/in_hand_scanner/icp.h>

#include <QVector>

#include <limits>
#include <cstdlib>
#include <iomanip>
#include <cmath>

#include <pcl/keypoints/iss_3d.h> ///////////keypoint
#include <pcl/keypoints/keypoint.h>
#include <pcl/keypoints/sift_keypoint.h>


#include <pcl/features/shot_omp.h>///////////feature
#include <pcl/features/normal_3d_omp.h>
#include <pcl/features/integral_image_normal.h>


//////////////
#include <pcl/common/transformation_from_correspondences.h>

#include <pcl/registration/correspondence_estimation.h>
#include <pcl/registration/correspondence_rejection.h>
#include <pcl/registration/correspondence_rejection_sample_consensus.h>

#include <pcl/registration/correspondence_rejection_one_to_one.h>
#include <pcl/registration/icp_nl.h>
#include <pcl/registration/sample_consensus_prerejective.h>
#include <pcl/registration/transformation_estimation_lm.h>
#include <pcl/registration/transformation_estimation_point_to_plane.h>
#include <pcl/registration/transformation_estimation_point_to_plane_weighted.h>
#include <pcl/registration/transformation_estimation_svd.h>
#include <pcl/registration/transforms.h>
#include <pcl/registration/icp.h>
/////////////


#include <pcl/search/kdtree.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/common/centroid.h>
#include <pcl/common/time.h>

#include <pcl/apps/in_hand_scanner/utils.h>

////////////////////////////////////////////////////////////////////////////////

pcl::ihs::ICP::ICP ()
  : kd_tree_ (new pcl::KdTreeFLANN <PointNormal> ()),

    epsilon_        (10e-6f),
    max_iterations_ (50),
    min_overlap_    (.75f),
    max_fitness_    (.1f),

    factor_ (9.f),
    max_angle_ (45.f)
{
}

////////////////////////////////////////////////////////////////////////////////

void
pcl::ihs::ICP::setEpsilon (const float epsilon) 
{
  if (epsilon > 0) epsilon_ = epsilon;
}

float
pcl::ihs::ICP::getEpsilon () const
{
  return (epsilon_);
}

////////////////////////////////////////////////////////////////////////////////

void
pcl::ihs::ICP::setMaxIterations (const unsigned int max_iter)
{
  max_iterations_ = max_iter < 1 ? 1 : max_iter;
}

unsigned int
pcl::ihs::ICP::getMaxIterations () const
{
  return (max_iterations_);
}

////////////////////////////////////////////////////////////////////////////////

void
pcl::ihs::ICP::setMinOverlap (const float overlap)
{
  min_overlap_ = pcl::ihs::clamp (overlap, 0.f, 1.f);
}

float
pcl::ihs::ICP::getMinOverlap () const
{
  return (min_overlap_);
}

////////////////////////////////////////////////////////////////////////////////

void
pcl::ihs::ICP::setMaxFitness (const float fitness)
{
  if (fitness > 0) max_fitness_ = fitness;
}

float
pcl::ihs::ICP::getMaxFitness () const
{
  return (max_fitness_);
}

////////////////////////////////////////////////////////////////////////////////

void
pcl::ihs::ICP::setCorrespondenceRejectionFactor (const float factor)
{
  factor_ = factor < 1.f ? 1.f : factor;
}

float
pcl::ihs::ICP::getCorrespondenceRejectionFactor () const
{
  return (factor_);
}

////////////////////////////////////////////////////////////////////////////////

void
pcl::ihs::ICP::setMaxAngle (const float angle)
{
  max_angle_ = pcl::ihs::clamp (angle, 0.f, 180.f);
}

float
pcl::ihs::ICP::getMaxAngle () const
{
  return (max_angle_);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

bool
pcl::ihs::ICP::findTransformation (const MeshConstPtr&              mesh_model,
                                   const CloudXYZRGBNormalConstPtr& cloud_data,
                                   const Eigen::Matrix4f&           T_init,
                                   Eigen::Matrix4f&                 T_final)
{
  // Check the input
  // TODO: Double check the minimum number of points necessary for icp
  const size_t n_min = 4;

  if(mesh_model->sizeVertices () < n_min || cloud_data->size () < n_min)
  {
    std::cerr << "ERROR in icp.cpp: Not enough input points!\n";
    return (false);
  }

  // Time measurements
  pcl::StopWatch sw;
  pcl::StopWatch sw_total;
  double t_select     = 0.;
  double t_build      = 0.;
  double t_nn_search  = 0.;
  double t_calc_trafo = 0.;

  // Convergence and registration failure
  float current_fitness  = 0.f;
  float previous_fitness = std::numeric_limits <float>::max ();
  float delta_fitness    = std::numeric_limits <float>::max ();
  float overlap          = std::numeric_limits <float>::quiet_NaN ();

  // Outlier rejection
  float squared_distance_threshold = std::numeric_limits<float>::max();

  // Transformation
  Eigen::Matrix4f T_cur = T_init; ////////////////////////////////////////////////////////////////////input

  // Point selection
  sw.reset ();
  const CloudNormalConstPtr cloud_model_selected = this->selectModelPoints (mesh_model, T_init.inverse ());
  const CloudNormalConstPtr cloud_data_selected  = this->selectDataPoints (cloud_data);
  t_select = sw.getTime ();

  const size_t n_model = cloud_model_selected->size ();
  const size_t n_data  = cloud_data_selected->size ();
  if(n_model < n_min) {std::cerr << "ERROR in icp.cpp: Not enough model points after selection!\n"; return (false);}
  if(n_data < n_min)  {std::cerr << "ERROR in icp.cpp: Not enough data points after selection!\n"; return (false);}



//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

  /////////////////////////////
  //////////keypoint///////////
  /////////////////////////////


  double model_resolution_Target = computeCloudResolution(cloud_model_selected);
  double model_resolution_Source = computeCloudResolution(cloud_data_selected);

  std::cout<<"model_resolution_Target :"<<model_resolution_Target<<"\n";
  std::cout<<"model_resolution_Source :"<<model_resolution_Source<<"\n";

  CloudNormalPtr target_cloud_kp (new CloudNormal ());
  CloudNormalPtr source_cloud_kp (new CloudNormal ());

  KeyPoint_Detector_ISS3D(  cloud_model_selected, model_resolution_Target, target_cloud_kp );
  KeyPoint_Detector_ISS3D(  cloud_data_selected,  model_resolution_Source, source_cloud_kp );

  const size_t n_model_kp = target_cloud_kp->size ();
  const size_t n_data_kp  = source_cloud_kp->size ();

  std::cout<<"n_model_kp :"<<n_model_kp<<"\n";
  std::cout<<"n_data_kp :"<<n_data_kp<<"\n";


  ////////////////////////////////////
  ////////////3D Feature//////////////
  ////////////////////////////////////

  pcl::PointCloud<Feature>::Ptr model_feat_3D( new pcl::PointCloud<Feature> );
  pcl::PointCloud<Feature>::Ptr data_feat_3D( new pcl::PointCloud<Feature> );

  //                                   searchSurface          inputNormals         KEYpt          model_resolution          FEAT__3D_OUT
  Feature_Descriptor_on_ISS3D(   cloud_model_selected, cloud_model_selected, target_cloud_kp, model_resolution_Target, model_feat_3D );
  Feature_Descriptor_on_ISS3D(   cloud_data_selected,  cloud_data_selected,  source_cloud_kp, model_resolution_Source, data_feat_3D );

  const size_t n_model_feat = model_feat_3D->size ();
  const size_t n_data_feat  = data_feat_3D->size ();

  std::cout<<"n_model_feat :"<<n_model_feat<<"\n";
  std::cout<<"n_data_feat :"<<n_data_feat<<"\n";


  /////////////////////////////////////////
  //////3D correspondense estimation///////
  /////////////////////////////////////////

  QVector<GeometrCORR> corr_feat_3D;

  Correspondence_Estimation(  data_feat_3D,
                              model_feat_3D,
                              source_cloud_kp,
                              target_cloud_kp,
                              corr_feat_3D  );  // OUT

  std::cout << "Correspondances Geometry : " << corr_feat_3D.size() <<"\n";


  ////////////////////////////////////
  //////initiate transformation///////
  ////////////////////////////////////

  Eigen::Matrix4f finalCloudTransformations;
  finalCloudTransformations = Eigen::Matrix4f::Identity();


//  pcl::transformPointCloudWithNormals( *cloud_data_selected, 
//                                      *cloud_data_selected, finalCloudTransformations );
                                                                                               
//  pcl::transformPointCloudWithNormals( *cloud_model_selected, 
//                                       *cloud_model_selected, finalCloudTransformations );
                                                
//  pcl::transformPointCloudWithNormals( *source_cloud_kp, 
//                                       *source_cloud_kp, finalCloudTransformations );  
 
//  pcl::transformPointCloudWithNormals( *target_cloud_kp, 
//                                      *target_cloud_kp, finalCloudTransformations );   





  ////////////////////////////////////
  //////estimate transformation///////
  ////////////////////////////////////

            Eigen::Matrix4f coarseTransformation = Eigen::Matrix4f::Identity();
            


//            if (corrFUSED_Filt->size() >= 4)
//            {
//                            pcl::registration::TransformationEstimationPointToPlaneWeighted< TYPE_Point_KEYpt, TYPE_Point_KEYpt > transEstALL;
//                            transEstALL.estimateRigidTransformation(*source_cloud_KEYfused,  // IN
//                                                                    *target_cloud_KEYfused,  // IN
//                                                                    *corrFUSED_Filt,         // IN
//                                                                    coarseTransformation ); // OUT
//            } 

//            std::cout << "\n\n" << "coarseTransformation" << "\n" << coarseTransformation << "\n" << std::endl;


                    
                    // //pcl::transformPointCloudWithNormals( *source_cloud_UNorg_PN, *source_cloud_UNorg_PN, coarseTransformation );
                    // //pcl::transformPointCloudWithNormals( *source_cloud_KEYp__3D, *source_cloud_KEYp__3D, coarseTransformation );
                    // //pcl::transformPointCloudWithNormals( *source_cloud_KEYfused, *source_cloud_KEYfused, coarseTransformation );
                    
                   

            






/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

  // Build a kd-tree
  sw.reset ();
  kd_tree_->setInputCloud (cloud_model_selected);
  t_build = sw.getTime ();

  std::vector <int>   index (1);
  std::vector <float> squared_distance (1);

  // Clouds with one to one correspondences
  CloudNormal cloud_model_corr;
  CloudNormal cloud_data_corr;

  cloud_model_corr.reserve (n_data);
  cloud_data_corr.reserve (n_data);

  // ICP main loop
  unsigned int iter = 1;
  PointNormal pt_d;
  const float dot_min = std::cos (max_angle_ * 17.45329252e-3); // deg to rad
  while (true)
  {
    // Accumulated error
    float squared_distance_sum = 0.f;

    // NN search
    cloud_model_corr.clear ();
    cloud_data_corr.clear ();
    sw.reset ();
    for (CloudNormal::const_iterator it_d = cloud_data_selected->begin (); it_d!=cloud_data_selected->end (); ++it_d)
    {
      // Transform the data point
      pt_d = *it_d;
      pt_d.getVector4fMap ()       = T_cur * pt_d.getVector4fMap ();
      pt_d.getNormalVector4fMap () = T_cur * pt_d.getNormalVector4fMap ();

      // Find the correspondence to the model points
      if (!kd_tree_->nearestKSearch (pt_d, 1, index, squared_distance))
      {
        std::cerr << "ERROR in icp.cpp: nearestKSearch failed!\n";
        return (false);
      }

      // Check the distance threshold
      if (squared_distance [0] < squared_distance_threshold)
      {
        if (index [0] >= cloud_model_selected->size ())
        {
          std::cerr << "ERROR in icp.cpp: Segfault!\n";
          std::cerr << "  Trying to access index " << index [0] << " >= " << cloud_model_selected->size () << std::endl;
          exit (EXIT_FAILURE);
        }

        const PointNormal& pt_m = cloud_model_selected->operator [] (index [0]);

        // Check the normals threshold
        if (pt_m.getNormalVector4fMap ().dot (pt_d.getNormalVector4fMap ()) > dot_min)
        {
          squared_distance_sum += squared_distance [0];

          cloud_model_corr.push_back (pt_m);
          cloud_data_corr.push_back (pt_d);
        }
      }
    }

    t_nn_search += sw.getTime ();

    const size_t n_corr = cloud_data_corr.size ();
    if (n_corr < n_min)
    {
      std::cerr << "ERROR in icp.cpp: Not enough correspondences: " << n_corr << " < " << n_min << std::endl;
      return (false);
    }

    // NOTE: The fitness is calculated with the transformation from the previous iteration (I don't re-calculate it after the transformation estimation). This means that the actual fitness will be one iteration "better" than the calculated fitness suggests. This should be no problem because the difference is small at the state of convergence.
    previous_fitness           = current_fitness;
    current_fitness            = squared_distance_sum / static_cast <float> (n_corr);
    delta_fitness              = std::abs (previous_fitness - current_fitness);
    squared_distance_threshold = factor_ * current_fitness;
    overlap                    = static_cast <float> (n_corr) / static_cast <float> (n_data);

    //    std::cerr << "Iter: " << std::left << std::setw(3) << iter
    //              << " | Overlap: " << std::setprecision(2) << std::setw(4) << overlap
    //              << " | current fitness: " << std::setprecision(5) << std::setw(10) << current_fitness
    //              << " | delta fitness: " << std::setprecision(5) << std::setw(10) << delta_fitness << std::endl;



    // Minimize the point to plane distance  ///////////////////////////////////////////////////////////////////////////////////////observe !!!
    sw.reset ();
    Eigen::Matrix4f T_delta = Eigen::Matrix4f::Identity ();
    Eigen::Matrix4f T1 = Eigen::Matrix4f::Identity ();
    Eigen::Matrix4f T2 = Eigen::Matrix4f::Identity ();
    Eigen::Matrix4f T3 = Eigen::Matrix4f::Identity ();
    if (!this->minimizePointPlane (cloud_data_corr, cloud_model_corr, T_delta, T1,T2,T3))
    {
      std::cerr << "ERROR in icp.cpp: minimizePointPlane failed!\n";
      return (false);
    }

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////observe !!!
    //std::cout<<"T1 :\n"<<T1<<"\n";
    //std::cout<<"T2 :\n"<<T2<<"\n";
    //std::cout<<"T3 :\n"<<T3<<"\n";

    t_calc_trafo += sw.getTime ();

    T_cur = T_delta * T_cur;

    // Convergence
    if (delta_fitness < epsilon_) break;
    ++iter;
    if (iter > max_iterations_)   break;

  } // End ICP main loop



  // Some output
  std::cerr << "Registration:\n"

            << "  - num model     / num data       : "
            << std::setw (8) << std::right << n_model << " / "
            << std::setw (8) << std::left  << n_data << "\n"

            << std::scientific << std::setprecision (1)

            << "  - delta fitness / epsilon        : "
            << std::setw (8) << std::right << delta_fitness << " / "
            << std::setw (8) << std::left  << epsilon_
            << (delta_fitness < epsilon_ ? " <-- :-)\n" : "\n")

            << "  - fitness       / max fitness    : "
            << std::setw (8) << std::right << current_fitness << " / "
            << std::setw (8) << std::left  << max_fitness_
            << (current_fitness > max_fitness_ ? " <-- :-(\n" : "\n")

            << std::fixed << std::setprecision (2)

            << "  - iter          / max iter       : "
            << std::setw (8) << std::right << iter << " / "
            << std::setw (8) << std::left  << max_iterations_
            << (iter > max_iterations_ ? " <-- :-(\n" : "\n")

            << "  - overlap       / min overlap    : "
            << std::setw (8) << std::right << overlap << " / "
            << std::setw (8) << std::left  << min_overlap_
            << (overlap < min_overlap_ ? " <-- :-(\n" : "\n")

            << std::fixed << std::setprecision (0)

            << "  - time select                    : "
            << std::setw (8) << std::right << t_select << " ms\n"

            << "  - time build kd-tree             : "
            << std::setw (8) << std::right << t_build << " ms\n"

            << "  - time nn-search / trafo / reject: "
            << std::setw (8) << std::right << t_nn_search << " ms\n"

            << "  - time minimize                  : "
            << std::setw (8) << std::right << t_calc_trafo << " ms\n"

            << "  - total time                     : "
            << std::setw (8) << std::right << sw_total.getTime () << " ms\n";

  if (iter > max_iterations_ || overlap <  min_overlap_ || current_fitness > max_fitness_)
  {
    return (false);
  }
  else if (delta_fitness <= epsilon_)
  {
    T_final = T_cur;         ////////////////////////////////////////////////////////////////////////////////////////////////////////////output
    return (true);
  }
  else
  {
    std::cerr << "ERROR in icp.cpp: Congratulations! you found a bug.\n";
    exit (EXIT_FAILURE);
  }
}





////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
double 
pcl::ihs::ICP::computeCloudResolution(const CloudNormalConstPtr &cloud)
{
      double res      = 0.0;
      int    n_points = 0;
      int    nres;

      pcl::search::KdTree<PointNormal> tree;

      tree.setInputCloud (cloud);

      for (int iii=0; iii<cloud->size(); ++iii)
      {
          ///////////////////////////////////////////////////////////////
          ///////////////////////////////////////////////////////////////
          if (  !pcl_isfinite( (*cloud)[iii].x )         ||     /////////
                !pcl_isfinite( (*cloud)[iii].y )         ||     /////////
                !pcl_isfinite( (*cloud)[iii].z )         ||     /////////
                !pcl_isfinite( (*cloud)[iii].normal_x )  ||     /////////
                !pcl_isfinite( (*cloud)[iii].normal_y )  ||     /////////
                !pcl_isfinite( (*cloud)[iii].normal_z )   )     continue;
          ///////////////////////////////////////////////////////////////
          ///////////////////////////////////////////////////////////////

          std::vector<int>   indices(       2 ); // dummy
          std::vector<float> sqr_distances( 2 );

          //Considering the second neighbor since the first is the point itself !!!
          nres = tree.nearestKSearch(iii, 2, indices, sqr_distances);
          if (nres == 2)
          {
            res += sqrt (sqr_distances[1]);
            ++n_points;
          }
      }

      //////////////////////////////////////
      if (n_points != 0)    res /= n_points;
      //////////////////////////////////////
      if (n_points == 0)    std::cout << std::endl << "computeCloudResolution - (n_points == 0) " << std::endl << std::endl;

      return res;
}

////////////////////////////////////////////////////////////////////////////////


void 
pcl::ihs::ICP::KeyPoint_Detector_ISS3D(const CloudNormalConstPtr  &cloud_in, 
                                       double                     &modelResolution_in, 
                                       CloudNormalPtr             &keypoint_out)    //////////////////输出不能是const，输入可以是!!!
{

        pcl::ISSKeypoint3D<PointNormal, PointNormal> keyPointDetector;

        pcl::search::KdTree<PointNormal>::Ptr tree (new pcl::search::KdTree<PointNormal>);
        keyPointDetector.setSearchMethod(tree);

        keyPointDetector.setSalientRadius(6 * modelResolution_in);   
        keyPointDetector.setNonMaxRadius(4 * modelResolution_in);      


        keyPointDetector.setThreshold21(0.975);
        keyPointDetector.setThreshold32(0.975);                    // Set the upper bound on the ratio between the third and the second eigenvalue.
        keyPointDetector.setMinNeighbors(5);                       // Set the minimum number of neighbors that has to be found while applying the non maxima suppression algorithm.
        keyPointDetector.setNumberOfThreads(4);                    // Initialize the scheduler and set the number of threads to use.

        keyPointDetector.setInputCloud(cloud_in);
        
        keyPointDetector.compute(*keypoint_out);

}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void
pcl::ihs::ICP::Feature_Descriptor_on_ISS3D( const CloudNormalConstPtr   &cloud__in,
                                            const CloudNormalConstPtr   &cloud___in,
                                            CloudNormalPtr              &keypoint__out,
                                            double                      &modelResolution__in,
                                            pcl::PointCloud<Feature>::Ptr             &Feat_3D_out )
{

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        pcl::SHOTEstimationOMP< PointNormal, PointNormal, Feature > featureDescriptor; // float descriptor [1344] || float rf [9]   // ISS3D
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        pcl::search::KdTree<PointNormal>::Ptr tree (new pcl::search::KdTree<PointNormal>);
        featureDescriptor.setSearchSurface( cloud__in );
        featureDescriptor.setInputNormals(  cloud___in );
        featureDescriptor.setInputCloud(    keypoint__out );
        featureDescriptor.setSearchMethod(  tree );
        featureDescriptor.setRadiusSearch(  6 * modelResolution__in ); // IMPORTANT: must be larger than radius used to estimate normals!!!
        featureDescriptor.compute( *Feat_3D_out );

}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void
pcl::ihs::ICP::Correspondence_Estimation(              const pcl::PointCloud<Feature>::Ptr       &data_feat_3D,   // corrEst.setInputSource
                                                       const pcl::PointCloud<Feature>::Ptr       &model_feat_3D,   // corrEst.setInputTarget
                                                       const CloudNormalPtr                      &data_kp_3D,
                                                       const CloudNormalPtr                      &model_kp_3D,
                                                       QVector<GeometrCORR>                      &corr3D)           // OUT
{

        pcl::CorrespondencesPtr  corr_NonFilt_feat_3D( new pcl::Correspondences );


        pcl::registration::CorrespondenceEstimation<Feature,Feature>   corrEst;
        corrEst.setInputSource( data_feat_3D );
        corrEst.setInputTarget( model_feat_3D );
        corrEst.determineCorrespondences( *corr_NonFilt_feat_3D );


        corr3D.clear();
        corr3D.resize( corr_NonFilt_feat_3D->size() );

        for (int iii=0; iii< corr_NonFilt_feat_3D->size(); iii++)
        {

            PointNormal ptSource = data_kp_3D->points[ (*corr_NonFilt_feat_3D)[iii].index_query ];
            PointNormal ptTarget = model_kp_3D->points[ (*corr_NonFilt_feat_3D)[iii].index_match ];

            corr3D[iii].source_3D_pt_Normal = ptSource;
            corr3D[iii].source_3D_pt_3f(0)       = corr3D[iii].source_3D_pt_Normal.x;
            corr3D[iii].source_3D_pt_3f(1)       = corr3D[iii].source_3D_pt_Normal.y;
            corr3D[iii].source_3D_pt_3f(2)       = corr3D[iii].source_3D_pt_Normal.z;
            corr3D[iii].source_3D_nrm3f(0)       = corr3D[iii].source_3D_pt_Normal.normal_x;
            corr3D[iii].source_3D_nrm3f(1)       = corr3D[iii].source_3D_pt_Normal.normal_y;
            corr3D[iii].source_3D_nrm3f(2)       = corr3D[iii].source_3D_pt_Normal.normal_z;

            corr3D[iii].target_3D_pt_Normal = ptTarget;
            corr3D[iii].target_3D_pt_3f(0)       = corr3D[iii].source_3D_pt_Normal.x;
            corr3D[iii].target_3D_pt_3f(1)       = corr3D[iii].source_3D_pt_Normal.y;
            corr3D[iii].target_3D_pt_3f(2)       = corr3D[iii].source_3D_pt_Normal.z;
            corr3D[iii].target_3D_nrm3f(0)       = corr3D[iii].source_3D_pt_Normal.normal_x;
            corr3D[iii].target_3D_nrm3f(1)       = corr3D[iii].source_3D_pt_Normal.normal_y;
            corr3D[iii].target_3D_nrm3f(2)       = corr3D[iii].source_3D_pt_Normal.normal_z;

        }

}














/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

pcl::ihs::ICP::CloudNormalConstPtr
pcl::ihs::ICP::selectModelPoints (const MeshConstPtr&    mesh_model,
                                  const Eigen::Matrix4f& T_inv) const
{
  const CloudNormalPtr cloud_model_out (new CloudNormal ());
  cloud_model_out->reserve (mesh_model->sizeVertices ());

  const Mesh::VertexDataCloud& cloud = mesh_model->getVertexDataCloud ();

  for (Mesh::VertexDataCloud::const_iterator it=cloud.begin (); it!=cloud.end (); ++it)
  {
    // Don't consider points that are facing away from the camera.
    if ((T_inv.lazyProduct (it->getNormalVector4fMap ())).z () < 0.f)
    {
      PointNormal pt;
      pt.getVector4fMap ()       = it->getVector4fMap ();
      pt.getNormalVector4fMap () = it->getNormalVector4fMap ();

      // NOTE: Not the transformed points!!
      cloud_model_out->push_back (pt);
    }
  }

  return (cloud_model_out);
}

////////////////////////////////////////////////////////////////////////////////

pcl::ihs::ICP::CloudNormalConstPtr
pcl::ihs::ICP::selectDataPoints (const CloudXYZRGBNormalConstPtr& cloud_data) const
{
  const CloudNormalPtr cloud_data_out (new CloudNormal ());
  cloud_data_out->reserve (cloud_data->size ());

  CloudXYZRGBNormal::const_iterator it_in = cloud_data->begin ();
  for (; it_in!=cloud_data->end (); ++it_in)
  {
    if (!boost::math::isnan (it_in->x))
    {
      PointNormal pt;
      pt.getVector4fMap ()       = it_in->getVector4fMap ();
      pt.getNormalVector4fMap () = it_in->getNormalVector4fMap ();

      cloud_data_out->push_back (pt);
    }
  }

  return (cloud_data_out);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////observe !!!

bool
pcl::ihs::ICP::minimizePointPlane (const CloudNormal& cloud_source,
                                   const CloudNormal& cloud_target,
                                   Eigen::Matrix4f&    T,
                                   Eigen::Matrix4f&    TT,
                                   Eigen::Matrix4f&    T_s,
                                   Eigen::Matrix4f&    T_t      ) const
{
  // Check the input
  // n < n_min already checked in the icp main loop
  const size_t n = cloud_source.size ();
  if (cloud_target.size () != n)
  {
    std::cerr << "ERROR in icp.cpp: Input must have the same size!\n";
    return (false);
  }

  // For numerical stability
  // - Low: Linear Least-Squares Optimization for Point-to-Plane ICP Surface Registration (2004), in the discussion: "To improve the numerical stability of the computation, it is important to use a unit of distance that is comparable in magnitude with the rotation angles. The simplest way is to rescale and move the two input surfaces so that they are bounded within a unit sphere or cube centered at the origin."
  // - Gelfand et al.: Geometrically Stable Sampling for the ICP Algorithm (2003), in sec 3.1: "As is common with PCA methods, we will shift the center of mass of the points to the origin." ... "Therefore, after shifting the center of mass, we will scale the point set so that the average distance of points from the origin is 1."
  // - Hartley, Zisserman: - Multiple View Geometry (2004), page 109: They normalize to sqrt(2)
  // TODO: Check the resulting C matrix for the conditioning.

  // Subtract the centroid and calculate the scaling factor
  Eigen::Vector4f c_s (0.f, 0.f, 0.f, 1.f);
  Eigen::Vector4f c_t (0.f, 0.f, 0.f, 1.f);
  pcl::compute3DCentroid (cloud_source, c_s); c_s.w () = 1.f;
  pcl::compute3DCentroid (cloud_target, c_t); c_t.w () = 1.f;

  // The normals are only needed for the target
  typedef std::vector <Eigen::Vector4f, Eigen::aligned_allocator <Eigen::Vector4f> > Vec4Xf;
  Vec4Xf xyz_s, xyz_t, nor_t;
  xyz_s.reserve (n);
  xyz_t.reserve (n);
  nor_t.reserve (n);

  CloudNormal::const_iterator it_s = cloud_source.begin ();
  CloudNormal::const_iterator it_t = cloud_target.begin ();

  float accum = 0.f;
  Eigen::Vector4f pt_s, pt_t;
  for (; it_s!=cloud_source.end (); ++it_s, ++it_t)
  {
    // Subtract the centroid
    pt_s = it_s->getVector4fMap () - c_s;
    pt_t = it_t->getVector4fMap () - c_t;

    xyz_s.push_back (pt_s);
    xyz_t.push_back (pt_t);
    nor_t.push_back (it_t->getNormalVector4fMap ());

    // Calculate the radius (L2 norm) of the bounding sphere through both shapes and accumulate the average
    // TODO: Change to squared norm and adapt the rest accordingly
    accum += pt_s.head <3> ().norm () + pt_t.head <3> ().norm ();
  }

  // Inverse factor (do a multiplication instead of division later)
  const float factor         = 2.f * static_cast <float> (n) / accum;
  const float factor_squared = factor*factor;

  // Covariance matrix C
  Eigen::Matrix <float, 6, 6> C;

  // Right hand side vector b
  Eigen::Matrix <float, 6, 1> b;

  // For Eigen vectorization: use 4x4 submatrixes instead of 3x3 submatrixes
  // -> top left 3x3 matrix will form the final C
  // Same for b
  Eigen::Matrix4f C_tl    = Eigen::Matrix4f::Zero(); // top left corner
  Eigen::Matrix4f C_tr_bl = Eigen::Matrix4f::Zero(); // top right / bottom left
  Eigen::Matrix4f C_br    = Eigen::Matrix4f::Zero(); // bottom right

  Eigen::Vector4f b_t     = Eigen::Vector4f::Zero(); // top
  Eigen::Vector4f b_b     = Eigen::Vector4f::Zero(); // bottom

  Vec4Xf::const_iterator it_xyz_s = xyz_s.begin ();
  Vec4Xf::const_iterator it_xyz_t = xyz_t.begin ();
  Vec4Xf::const_iterator it_nor_t = nor_t.begin ();

  Eigen::Vector4f cross;
  float dot;
  for (; it_xyz_s!=xyz_s.end (); ++it_xyz_s, ++it_xyz_t, ++it_nor_t)
  {
    cross    = it_xyz_s->cross3 (*it_nor_t);

    C_tl    += cross     * cross.    transpose ();
    C_tr_bl += cross     * it_nor_t->transpose ();
    C_br    += *it_nor_t * it_nor_t->transpose ();

    dot      = (*it_xyz_t-*it_xyz_s).dot (*it_nor_t);

    b_t     += cross     * dot;
    b_b     += *it_nor_t * dot;
  }

  // Scale with the factor and copy the 3x3 submatrixes into C and b
  C_tl    *= factor_squared;
  C_tr_bl *= factor;

  C << C_tl.  topLeftCorner <3, 3> ()            , C_tr_bl.topLeftCorner <3, 3> (),
      C_tr_bl.topLeftCorner <3, 3> ().transpose(), C_br.   topLeftCorner <3, 3> ();

  b << b_t.head <3> () * factor_squared,
      b_b. head <3> () * factor;

  // Solve C * x = b with a Cholesky factorization with pivoting
  // x = [alpha; beta; gamma; trans_x; trans_y; trans_z]
  Eigen::Matrix <float, 6, 1> x = C.selfadjointView <Eigen::Lower> ().ldlt ().solve (b);

  ///////////////////////////////////////////////// The calculated transformation in the scaled coordinate system (T1)
  const float
      sa = std::sin (x (0)),
      ca = std::cos (x (0)),
      sb = std::sin (x (1)),
      cb = std::cos (x (1)),
      sg = std::sin (x (2)),
      cg = std::cos (x (2)),
      tx = x (3),
      ty = x (4),
      tz = x (5);

  //Eigen::Matrix4f TT;
  TT << cg*cb, -sg*ca+cg*sb*sa,  sg*sa+cg*sb*ca, tx,
      sg*cb  ,  cg*ca+sg*sb*sa, -cg*sa+sg*sb*ca, ty,
      -sb    ,  cb*sa         ,  cb*ca         , tz,
      0.f    ,  0.f           ,  0.f           , 1.f;

  /////////////////////////////////////////////// Transformation matrixes into the local coordinate systems of model/data (T2/T3)
  //Eigen::Matrix4f T_s, T_t;

  T_s << factor, 0.f   , 0.f   , -c_s.x () * factor,
      0.f      , factor, 0.f   , -c_s.y () * factor,
      0.f      , 0.f   , factor, -c_s.z () * factor,
      0.f      , 0.f   , 0.f   ,  1.f;

  T_t << factor, 0.f   , 0.f   , -c_t.x () * factor,
      0.f      , factor, 0.f   , -c_t.y () * factor,
      0.f      , 0.f   , factor, -c_t.z () * factor,
      0.f      , 0.f   , 0.f   ,  1.f;

  // Output transformation T
  //T = TT;
  T = T_t.inverse () * TT * T_s;                   // T = inv(T_t)*TT*T_s 

  return (true);
}

////////////////////////////////////////////////////////////////////////////////