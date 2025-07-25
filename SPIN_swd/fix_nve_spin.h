/* -*- c++ -*- ----------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://www.lammps.org/, Sandia National Laboratories
   LAMMPS development team: developers@lammps.org

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#ifdef FIX_CLASS
// clang-format off
FixStyle(nve/spin,FixNVESpin);
// clang-format on
#else

#ifndef LMP_FIX_NVE_SPIN_H
#define LMP_FIX_NVE_SPIN_H

#include "fix.h"

namespace LAMMPS_NS {

class FixNVESpin : public Fix {
  friend class PairSpin;

 public:
  FixNVESpin(class LAMMPS *, int, char **);
  ~FixNVESpin() override;
  int setmask() override;
  void init() override;
  void initial_integrate(int) override;
  void final_integrate() override;

  // Modified by Weidi
  void ComputeInteractionsSpin(int, double *);    // compute and advance single spin functions
  void ComputeInteractionsSpin_one_side(int, double *);
  void AdvanceSingleSpin(int);
  void AdvanceSingleSpin_no_langevin(int, double *, double *);
  void AdvanceSingleSpin_one_side(int, double *, double *);
  //
  
  void sectoring();    // sectoring operation functions
  int coords2sector(double *);

  void setup_pre_neighbor() override;
  void pre_neighbor() override;

  int lattice_flag;    // lattice_flag = 0 if spins only
                       // lattice_flag = 1 if spin-lattice calc.

 protected:
  int sector_flag;    // sector_flag = 0  if serial algorithm
                      // sector_flag = 1  if parallel algorithm

  double dtv, dtf, dts;    // velocity, force, and spin timesteps

  int nlocal_max;    // max value of nlocal (for size of lists)

  int pair_spin_flag;          // magnetic pair flags
  int long_spin_flag;          // magnetic long-range flag
  int precession_spin_flag;    // magnetic precession flags
  int maglangevin_flag;        // magnetic langevin flags
  int tdamp_flag, temp_flag;
  int setforce_spin_flag;

  // pointers to magnetic pair styles

  int npairs, npairspin;    // # of pairs, and # of spin pairs
  class Pair *pair;
  class PairSpin **spin_pairs;    // vector of spin pairs

  // pointers to fix langevin/spin styles

  int nlangspin;
  class FixLangevinSpin **locklangevinspin;

  // pointers to fix setforce/spin styles

  int nsetspin;
  class FixSetForceSpin *locksetforcespin;    // to be done

  // pointers to fix precession/spin styles

  int nprecspin;
  class FixPrecessionSpin **lockprecessionspin;

  // sectoring variables

  int nsectors;
  double *rsec;

  // stacking variables for sectoring algorithm

  int *stack_head;         // index of first atom in backward_stacks
  int *stack_foot;         // index of first atom in forward_stacks
  int *backward_stacks;    // index of next atom in backward stack
  int *forward_stacks;     // index of next atom in forward stack
};

}    // namespace LAMMPS_NS

#endif
#endif
