/* Copyright (c) 2008-2019 the MRtrix3 contributors.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Covered Software is provided under this License on an "as is"
 * basis, without warranty of any kind, either expressed, implied, or
 * statutory, including, without limitation, warranties that the
 * Covered Software is free of defects, merchantable, fit for a
 * particular purpose or non-infringing.
 * See the Mozilla Public License v. 2.0 for more details.
 *
 * For more details, see http://www.mrtrix.org/.
 */

#include <Eigen/Dense>

#include "command.h"
#include "header.h"
#include "image.h"
#include "image_helpers.h"
#include "types.h"

#include "math/constrained_least_squares.h"
#include "math/rng.h"
#include "math/sphere.h"
#include "math/SH.h"
#include "math/ZSH.h"

#include "dwi/gradient.h"
#include "dwi/shells.h"



using namespace MR;
using namespace App;



//#define AMP2RESPONSE_DEBUG
//#define AMP2RESPONSE_PERVOXEL_IMAGES



void usage ()
{

  AUTHOR = "Robert E. Smith (robert.smith@florey.edu.au)";

  SYNOPSIS = "Estimate response function coefficients based on the DWI signal in single-fibre voxels";

  DESCRIPTION
   + "This command uses the image data from all selected single-fibre voxels concurrently, "
     "rather than simply averaging their individual spherical harmonic coefficients. It also "
     "ensures that the response function is non-negative, and monotonic (i.e. its amplitude "
     "must increase from the fibre direction out to the orthogonal plane)."

   + "If multi-shell data are provided, and one or more b-value shells are not explicitly "
     "requested, the command will generate a response function for every b-value shell "
     "(including b=0 if present).";

  ARGUMENTS
    + Argument ("amps", "the amplitudes image").type_image_in()
    + Argument ("mask", "the mask containing the voxels from which to estimate the response function").type_image_in()
    + Argument ("directions", "a 4D image containing the estimated fibre directions").type_image_in()
    + Argument ("response", "the output zonal spherical harmonic coefficients").type_file_out();

  OPTIONS
    + Option ("isotropic", "estimate an isotropic response function (lmax=0 for all shells)")

    + Option ("noconstraint", "disable the non-negativity and monotonicity constraints")

    + Option ("directions", "provide an external text file containing the directions along which the amplitudes are sampled")
      + Argument("path").type_file_in()

    + DWI::ShellsOption

    + Option ("lmax", "specify the maximum harmonic degree of the response function to estimate "
                      "(can be a comma-separated list for multi-shell data)")
      + Argument ("values").type_sequence_int();

  REFERENCES
    + "Smith, R. E.; Dhollander, T. & Connelly, A. " // Internal
      "Constrained linear least squares estimation of anisotropic response function for spherical deconvolution. "
      "ISMRM Workshop on Breaking the Barriers of Diffusion MRI, 23.";
}



Eigen::Matrix<default_type, 3, 3> gen_rotation_matrix (const Eigen::Vector3& dir)
{
  static Math::RNG::Normal<default_type> rng;
  // Generates a matrix that will rotate a unit vector into a new frame of reference,
  //   where the peak direction of the FOD is aligned in Z (3rd dimension)
  // Previously this was done using the tensor eigenvectors
  // Here the other two axes are determined at random (but both are orthogonal to the FOD peak direction)
  Eigen::Matrix<default_type, 3, 3> R;
  R (2, 0) = dir[0]; R (2, 1) = dir[1]; R (2, 2) = dir[2];
  Eigen::Vector3 vec2 (rng(), rng(), rng());
  vec2 = dir.cross (vec2);
  vec2.normalize();
  R (0, 0) = vec2[0]; R (0, 1) = vec2[1]; R (0, 2) = vec2[2];
  Eigen::Vector3 vec3 = dir.cross (vec2);
  vec3.normalize();
  R (1, 0) = vec3[0]; R (1, 1) = vec3[1]; R (1, 2) = vec3[2];
  return R;
}


vector<size_t> all_volumes (const size_t num)
{
  vector<size_t> result (num);
  for (size_t i = 0; i != num; ++i)
    result[i] = i;
  return result;
}




class Accumulator {
  public:
    class Shared {
      public:
        Shared (int lmax, const vector<size_t>& volumes, const Eigen::MatrixXd& dirs) :
          lmax (lmax),
          dirs (dirs),
          volumes (volumes),
          M (Eigen::MatrixXd::Zero (Math::ZSH::NforL (lmax), Math::ZSH::NforL (lmax))),
          b (Eigen::VectorXd::Zero (M.rows())),
          count (0) { }

        const int lmax;
        const Eigen::MatrixXd& dirs;
        const vector<size_t>& volumes;
        Eigen::MatrixXd M;
        Eigen::VectorXd b;
        size_t count;
    };

    Accumulator (Shared& shared) :
      S (shared),
      signals (S.volumes.size()),
      b (S.b),
      M (S.M),
      count (0),
      rotated_dirs_cartesian (S.dirs.rows(), 3) { }

    ~Accumulator ()
    {
      // accumulate results from all threads:
      S.M += M;
      S.b += b;
      S.count += count;
    }

    void operator() (Image<float>& signal_image, Image<float>& dir_image, Image<bool>& mask)
    {
      if (mask.value()) {
        ++count;

        // Grab the fibre direction
        Eigen::Vector3 fibre_dir;
        for (dir_image.index(3) = 0; dir_image.index(3) != 3; ++dir_image.index(3))
          fibre_dir[dir_image.index(3)] = dir_image.value();
        fibre_dir.normalize();

        // Rotate the directions into a new reference frame,
        //   where the Z axis is defined by the specified direction
        auto R = gen_rotation_matrix (fibre_dir);
        rotated_dirs_cartesian.transpose() = R * S.dirs.transpose();

        // Convert directions from Euclidean space to azimuth/elevation pairs
        Eigen::MatrixXd rotated_dirs_azel = Math::Sphere::cartesian2spherical (rotated_dirs_cartesian);

        // Constrain elevations to between 0 and pi/2
        for (ssize_t i = 0; i != rotated_dirs_azel.rows(); ++i) {
          if (rotated_dirs_azel (i, 1) > Math::pi_2) {
            if (rotated_dirs_azel (i, 0) > Math::pi)
              rotated_dirs_azel (i, 0) -= Math::pi;
            else
              rotated_dirs_azel (i, 0) += Math::pi;
            rotated_dirs_azel (i, 1) = Math::pi - rotated_dirs_azel (i, 1);
          }
        }

        // Generate the ZSH -> amplitude transform
        transform = Math::ZSH::init_amp_transform<default_type> (rotated_dirs_azel.col(1), S.lmax);

        // Grab the image data
        for (size_t i = 0; i != S.volumes.size(); ++i) {
          signal_image.index(3) = S.volumes[i];
          signals[i] = signal_image.value();
        }

        // accumulate results:
        b += transform.transpose() * signals;
        M.selfadjointView<Eigen::Lower>().rankUpdate (transform.transpose());
      }
    }

  protected:
    Shared& S;
    Eigen::VectorXd signals, b;
    Eigen::MatrixXd M, transform;
    size_t count;
    Eigen::Matrix<default_type, Eigen::Dynamic, 3> rotated_dirs_cartesian;
};


void run ()
{

  // Get directions from either selecting a b-value shell, or the header, or external file
  auto header = Header::open (argument[0]);

  // May be dealing with multiple shells
  vector<Eigen::MatrixXd> dirs_azel;
  vector<vector<size_t>> volumes;
  std::unique_ptr<DWI::Shells> shells;

  auto opt = get_options ("directions");
  if (opt.size()) {
    dirs_azel.push_back (load_matrix (opt[0][0]));
    volumes.push_back (all_volumes (dirs_azel.size()));
  } else {
    auto hit = header.keyval().find ("directions");
    if (hit != header.keyval().end()) {
      vector<default_type> dir_vector;
      for (auto line : split_lines (hit->second)) {
        auto v = parse_floats (line);
        dir_vector.insert (dir_vector.end(), v.begin(), v.end());
      }
      Eigen::MatrixXd directions (dir_vector.size() / 2, 2);
      for (size_t i = 0; i < dir_vector.size(); i += 2) {
        directions (i/2, 0) = dir_vector[i];
        directions (i/2, 1) = dir_vector[i+1];
      }
      dirs_azel.push_back (std::move (directions));
      volumes.push_back (all_volumes (dirs_azel.size()));
    } else {
      auto grad = DWI::get_valid_DW_scheme (header);
      shells.reset (new DWI::Shells (grad));
      shells->select_shells (false, false, false);
      for (size_t i = 0; i != shells->count(); ++i) {
        volumes.push_back ((*shells)[i].get_volumes());
        dirs_azel.push_back (DWI::gen_direction_matrix (grad, volumes.back()));
      }
    }
  }

  vector<int> lmax;
  int max_lmax = 0;
  opt = get_options ("lmax");
  if (get_options("isotropic").size()) {
    for (size_t i = 0; i != dirs_azel.size(); ++i)
      lmax.push_back (0);
    max_lmax = 0;
  } else if (opt.size()) {
    lmax = parse_ints (opt[0][0]);
    if (lmax.size() != dirs_azel.size())
      throw Exception ("Number of lmax\'s specified (" + str(lmax.size()) + ") does not match number of b-value shells (" + str(dirs_azel.size()) + ")");
    for (auto i : lmax) {
      if (i < 0)
        throw Exception ("Values specified for lmax must be non-negative");
      if (i%2)
        throw Exception ("Values specified for lmax must be even");
      max_lmax = std::max (max_lmax, i);
    }
    if ((*shells)[0].is_bzero() && lmax.front()) {
      WARN ("Non-zero lmax requested for " +
            ((*shells)[0].get_mean() ?
            "first shell (mean b=" + str((*shells)[0].get_mean()) + "), which MRtrix3 has classified as b=0;" :
            "b=0 shell;"));
      WARN ("  unless intended, this is likely to fail, as b=0 contains no orientation contrast");
    }
  } else {
    // Auto-fill lmax
    // Because the amp->SH transform doesn't need to be applied per voxel,
    //   lmax is effectively unconstrained. Therefore generate response at
    //   lmax=10 regardless of number of input volumes.
    // - UNLESS it's b=0, in which case force lmax=0
    for (size_t i = 0; i != dirs_azel.size(); ++i) {
      if (!i && shells && shells->smallest().is_bzero())
        lmax.push_back (0);
      else
        lmax.push_back (10);
    }
    max_lmax = (shells && shells->smallest().is_bzero() && lmax.size() == 1) ? 0 : 10;
  }

  auto image = header.get_image<float>();
  auto mask = Image<bool>::open (argument[1]);
  check_dimensions (image, mask, 0, 3);
  if (!(mask.ndim() == 3 || (mask.ndim() == 4 && mask.size(3) == 1)))
    throw Exception ("input mask must be a 3D image");
  auto dir_image = Image<float>::open (argument[2]);
  if (dir_image.ndim() < 4 || dir_image.size(3) < 3)
    throw Exception ("input direction image \"" + std::string (argument[2]) + "\" does not have expected dimensions");
  check_dimensions (image, dir_image, 0, 3);

  size_t num_voxels = 0;
  for (auto l = Loop (mask, 0, 3) (mask); l; ++l) {
    if (mask.value())
      ++num_voxels;
  }
  if (!num_voxels)
    throw Exception ("input mask does not contain any voxels");

  Eigen::MatrixXd responses (dirs_azel.size(), Math::ZSH::NforL (max_lmax));

  for (size_t shell_index = 0; shell_index != dirs_azel.size(); ++shell_index) {

    std::string shell_desc = (dirs_azel.size() > 1) ? ("_shell" + str(shell_index)) : "";

    // check the ZSH -> amplitude transform:
    {
      auto transform = Math::ZSH::init_amp_transform<default_type> (dirs_azel[shell_index].col(1), lmax[shell_index]);
      if (!transform.allFinite()) {
        Exception e ("Unable to construct A2SH transformation for shell b=" + str(int(std::round((*shells)[shell_index].get_mean()))) + ";");
        e.push_back ("  lmax (" + str(lmax[shell_index]) + ") may be too large for this shell");
        if (!shell_index && (*shells)[0].is_bzero())
          e.push_back ("  (this appears to be a b=0 shell, and therefore lmax should be set to 0 for this shell)");
        throw e;
      }
    }


    auto dirs_cartesian = Math::Sphere::spherical2cartesian (dirs_azel[shell_index]);

    Accumulator::Shared shared (lmax[shell_index], volumes[shell_index], dirs_cartesian);
    ThreadedLoop(image, 0, 3).run (Accumulator (shared), image, dir_image, mask);

/*
    // All directions from all SF voxels get concatenated into a single large matrix
    Eigen::MatrixXd cat_transforms (num_voxels * dirs_azel[shell_index].rows(), Math::ZSH::NforL (lmax[shell_index]));
    Eigen::VectorXd cat_data (num_voxels * dirs_azel[shell_index].rows());

#ifdef AMP2RESPONSE_DEBUG
    // To make sure we've got our data rotated correctly, let's generate a scatterplot of
    //   elevation vs. amplitude
    Eigen::MatrixXd scatter;
#endif

    size_t voxel_counter = 0;
    for (auto l = Loop (mask, 0, 3) (image, mask, dir_image); l; ++l) {
      if (mask.value()) {

        // Grab the image data
        Eigen::VectorXd data (dirs_azel[shell_index].rows());
        for (size_t i = 0; i != volumes[shell_index].size(); ++i) {
          image.index(3) = volumes[shell_index][i];
          data[i] = image.value();
        }

        // Grab the fibre direction
        Eigen::Vector3 fibre_dir;
        for (dir_image.index(3) = 0; dir_image.index(3) != 3; ++dir_image.index(3))
          fibre_dir[dir_image.index(3)] = dir_image.value();
        fibre_dir.normalize();

        // Rotate the directions into a new reference frame,
        //   where the Z axis is defined by the specified direction
        Eigen::Matrix<default_type, 3, 3> R = gen_rotation_matrix (fibre_dir);
        Eigen::Matrix<default_type, Eigen::Dynamic, 3> rotated_dirs_cartesian (dirs_cartesian.rows(), 3);
        Eigen::Vector3 vec (3), rot (3);
        for (ssize_t row = 0; row != dirs_azel[shell_index].rows(); ++row) {
          vec = dirs_cartesian.row (row);
          rot = R * vec;
          rotated_dirs_cartesian.row (row) = rot;
        }

        // Convert directions from Euclidean space to azimuth/elevation pairs
        Eigen::MatrixXd rotated_dirs_azel = Math::Sphere::cartesian2spherical (rotated_dirs_cartesian);

        // Constrain elevations to between 0 and pi/2
        for (ssize_t i = 0; i != rotated_dirs_azel.rows(); ++i) {
          if (rotated_dirs_azel (i, 1) > Math::pi_2) {
            if (rotated_dirs_azel (i, 0) > Math::pi)
              rotated_dirs_azel (i, 0) -= Math::pi;
            else
              rotated_dirs_azel (i, 0) += Math::pi;
            rotated_dirs_azel (i, 1) = Math::pi - rotated_dirs_azel (i, 1);
          }
        }

#ifdef AMP2RESPONSE_PERVOXEL_IMAGES
        // For the sake of generating a figure, output the original and rotated signals to a dixel ODF image
        Header rotated_header (header);
        rotated_header.size(0) = rotated_header.size(1) = rotated_header.size(2) = 1;
        rotated_header.size(3) = volumes[shell_index].size();
        Header nonrotated_header (rotated_header);
        nonrotated_header.size(3) = header.size(3);
        Eigen::MatrixXd rotated_grad (volumes[shell_index].size(), 4);
        for (size_t i = 0; i != volumes.size(); ++i) {
          rotated_grad.block<1,3>(i, 0) = rotated_dirs_cartesian.row(i);
          rotated_grad(i, 3) = 1000.0;
        }
        DWI::set_DW_scheme (rotated_header, rotated_grad);
        Image<float> out_rotated = Image<float>::create ("rotated_amps_" + str(sf_counter) + shell_desc + ".mif", rotated_header);
        Image<float> out_nonrotated = Image<float>::create ("nonrotated_amps_" + str(sf_counter) + shell_desc + ".mif", nonrotated_header);
        out_rotated.index(0) = out_rotated.index(1) = out_rotated.index(2) = 0;
        out_nonrotated.index(0) = out_nonrotated.index(1) = out_nonrotated.index(2) = 0;
        for (size_t i = 0; i != volumes[shell_index].size(); ++i) {
          image.index(3) = volumes[shell_index][i];
          out_rotated.index(3) = i;
          out_rotated.value() = image.value();
        }
        for (ssize_t i = 0; i != header.size(3); ++i) {
          image.index(3) = out_nonrotated.index(3) = i;
          out_nonrotated.value() = image.value();
        }
#endif

        // Generate the ZSH -> amplitude transform
        Eigen::MatrixXd transform = Math::ZSH::init_amp_transform<default_type> (rotated_dirs_azel.col(1), lmax[shell_index]);
        if (!transform.allFinite()) {
          Exception e ("Unable to construct A2SH transformation for shell b=" + str(int(std::round((*shells)[shell_index].get_mean()))) + ";");
          e.push_back ("  lmax (" + str(lmax[shell_index]) + ") may be too large for this shell");
          if (!shell_index && (*shells)[0].is_bzero())
            e.push_back ("  (this appears to be a b=0 shell, and therefore lmax should be set to 0 for this shell)");
          throw e;
        }

        // Concatenate these data to the ICLS matrices
        cat_transforms.block (voxel_counter * data.size(), 0, transform.rows(), transform.cols()) = transform;
        cat_data.segment (voxel_counter * data.size(), data.size()) = data;

#ifdef AMP2RESPONSE_DEBUG
        scatter.conservativeResize (cat_data.size(), 2);
        scatter.block (old_rows, 0, data.size(), 1) = rotated_dirs_azel.col(1);
        scatter.block (old_rows, 1, data.size(), 1) = data;
#endif

        ++voxel_counter;

      }
    }

#ifdef AMP2RESPONSE_DEBUG
    save_matrix (scatter, "scatter" + shell_desc + ".csv");
#endif
*/

    Eigen::VectorXd rf;
    shell_desc = (shells && shells->count() > 1) ? ("Shell b=" + str(int(std::round((*shells)[shell_index].get_mean()))) + ": ") : "";
    // Is this anything other than an isotropic response?
    if (lmax[shell_index]) {

      if (get_options("noconstraint").size()) {

        rf = shared.M.llt().solve (shared.b);

        CONSOLE (shell_desc + "Response function [" + str(rf.transpose().cast<float>()) + "] solved via ordinary least-squares from " + str(shared.count) + " voxels");

/*
        // Get an ordinary least squares solution
        Eigen::HouseholderQR<Eigen::MatrixXd> solver (cat_transforms);
        rf = solver.solve (cat_data);

        CONSOLE (shell_desc + "Response function [" + str(rf.transpose().cast<float>()) + "] solved via ordinary least-squares from " + str(voxel_counter) + " voxels");
        */

      } else {
/*
        // Generate the constraint matrix
        // We are going to both constrain the amplitudes to be non-negative, and constrain the derivatives to be non-negative
        const size_t num_angles_constraint = 90;
        Eigen::VectorXd els;
        els.resize (num_angles_constraint+1);
        for (size_t i = 0; i <= num_angles_constraint; ++i)
          els[i] = default_type(i) * Math::pi / 180.0;
        Eigen::MatrixXd amp_transform   = Math::ZSH::init_amp_transform  <default_type> (els, lmax[shell_index]);
        Eigen::MatrixXd deriv_transform = Math::ZSH::init_deriv_transform<default_type> (els, lmax[shell_index]);

        Eigen::MatrixXd constraints (amp_transform.rows() + deriv_transform.rows(), amp_transform.cols());
        constraints.block (0, 0, amp_transform.rows(), amp_transform.cols()) = amp_transform;
        constraints.block (amp_transform.rows(), 0, deriv_transform.rows(), deriv_transform.cols()) = deriv_transform;

        // Initialise the problem solver
        auto problem = Math::ICLS::Problem<default_type> (cat_transforms, constraints, 1e-10, 1e-10);
        auto solver  = Math::ICLS::Solver <default_type> (problem);

        // Estimate the solution
        const size_t niter = solver (rf, cat_data);

        CONSOLE (shell_desc + "Response function [" + str(rf.transpose().cast<float>()) + " ] solved after " + str(niter) + " constraint iterations from " + str(voxel_counter) + " voxels");
*/

      }
    } else {

      // lmax is zero - perform a straight average of the image data
      rf.resize(1);
      rf[0] = shared.b(0) / shared.M(0,0);

      //CONSOLE (shell_desc + "Response function [ " + str(float(rf[0])) + " ] from average of " + str(voxel_counter) + " voxels");
      CONSOLE (shell_desc + "Response function [ " + str(float(rf[0])) + " ] from average of " + str(shared.count) + " voxels");

    }

    rf.conservativeResizeLike (Eigen::VectorXd::Zero (Math::ZSH::NforL (max_lmax)));
    responses.row(shell_index) = rf;
  }

  save_matrix (responses, argument[3]);
}
