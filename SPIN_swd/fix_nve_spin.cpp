// clang-format off
/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://www.lammps.org/, Sandia National Laboratories
   LAMMPS development team: developers@lammps.org

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------
   Contributing authors: Julien Tranchida (SNL)
                         Aidan Thompson (SNL)

   Please cite the related publication:
   Tranchida, J., Plimpton, S. J., Thibaudeau, P., & Thompson, A. P. (2018).
   Massively parallel symplectic algorithm for coupled magnetic spin dynamics
   and molecular dynamics. Journal of Computational Physics.
------------------------------------------------------------------------- */

#include "fix_nve_spin.h"
#include "math_const.h"
#include "atom.h"
#include "citeme.h"
#include "comm.h"
#include "domain.h"
#include "error.h"
#include "fix_langevin_spin.h"
#include "fix_precession_spin.h"
#include "fix_setforce_spin.h"
#include "force.h"
#include "memory.h"
#include "modify.h"
#include "pair_hybrid.h"
#include "pair_spin.h"
#include "update.h"

#include "group.h"
#include <iostream>
//#include <fstream>
#include <cstring>

using namespace LAMMPS_NS;
using namespace FixConst;
using namespace MathConst;
using namespace std;

static const char cite_fix_nve_spin[] =
  "fix nve/spin command: doi:10.1016/j.jcp.2018.06.042\n\n"
  "@article{tranchida2018massively,\n"
  "title={Massively Parallel Symplectic Algorithm for Coupled Magnetic Spin "
  "   Dynamics and Molecular Dynamics},\n"
  "author={Tranchida, J and Plimpton, S J and Thibaudeau, P and Thompson, A P},\n"
  "journal={Journal of Computational Physics},\n"
  "volume={372},\n"
  "pages={406--425},\n"
  "year={2018},\n"
  "publisher={Elsevier}\n"
  "doi={10.1016/j.jcp.2018.06.042}\n"
  "}\n\n";

enum{NONE};

/* ---------------------------------------------------------------------- */

FixNVESpin::FixNVESpin(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg),
  pair(nullptr), spin_pairs(nullptr), locklangevinspin(nullptr),
  locksetforcespin(nullptr), lockprecessionspin(nullptr),
  rsec(nullptr), stack_head(nullptr), stack_foot(nullptr),
  backward_stacks(nullptr), forward_stacks(nullptr)
{
  if (lmp->citeme) lmp->citeme->add(cite_fix_nve_spin);

  if (narg < 4) error->all(FLERR,"Illegal fix/nve/spin command");

  time_integrate = 1;
  sector_flag = NONE;
  lattice_flag = 1;
  nlocal_max = 0;
  npairs = 0;
  npairspin = 0;

  // test nprec
  nprecspin = nlangspin = nsetspin = 0;

  // checking if map array or hash is defined

  if (atom->map_style == Atom::MAP_NONE)
    error->all(FLERR,"Fix nve/spin requires an atom map, see atom_modify");

  // defining sector_flag

  int nprocs_tmp = comm->nprocs;
  if (nprocs_tmp == 1) {
    sector_flag = 0;
  } else if (nprocs_tmp >= 1) {
    sector_flag = 1;
  } else error->all(FLERR,"Illegal fix/nve/spin command");

  // defining lattice_flag

  // changing the lattice option, from (yes,no) -> (moving,frozen)
  // for now, (yes,no) still works (to avoid user's confusions).

  int iarg = 3;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"lattice") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal fix/nve/spin command");
      const std::string latarg = arg[iarg+1];
      if ((latarg == "no") || (latarg == "off") || (latarg == "false") || (latarg == "frozen"))
        lattice_flag = 0;
      else if ((latarg == "yes") || (latarg == "on") || (latarg == "true") || (latarg == "moving"))
        lattice_flag = 1;
      else error->all(FLERR,"Illegal fix/nve/spin command");
      iarg += 2;
    } else error->all(FLERR,"Illegal fix/nve/spin command");
  }

  // check if the atom/spin style is defined

  if (!atom->sp_flag)
    error->all(FLERR,"Fix nve/spin requires atom/spin style");

  // check if sector_flag is correctly defined

  if (sector_flag == 0 && nprocs_tmp > 1)
    error->all(FLERR,"Illegal fix/nve/spin command");

  // initialize the magnetic interaction flags

  pair_spin_flag = 0;
  long_spin_flag = 0;
  precession_spin_flag = 0;
  maglangevin_flag = 0;
  tdamp_flag = temp_flag = 0;
  setforce_spin_flag = 0;
}

/* ---------------------------------------------------------------------- */

FixNVESpin::~FixNVESpin()
{
  memory->destroy(rsec);
  memory->destroy(stack_head);
  memory->destroy(stack_foot);
  memory->destroy(forward_stacks);
  memory->destroy(backward_stacks);
  delete [] spin_pairs;
  delete [] locklangevinspin;
  delete [] lockprecessionspin;
}

/* ---------------------------------------------------------------------- */

int FixNVESpin::setmask()
{
  int mask = 0;
  mask |= INITIAL_INTEGRATE;
  mask |= PRE_NEIGHBOR;
  mask |= FINAL_INTEGRATE;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixNVESpin::init()
{

  // set timesteps

  dtv = update->dt;
  dtf = 0.5 * update->dt * force->ftm2v;
  dts = 0.25 * update->dt;
  npairs = npairspin = 0;

  // set ptrs on Pair/Spin styles

  // loop 1: obtain # of Pairs, and # of Pair/Spin styles

  npairspin = 0;
  PairHybrid *hybrid = dynamic_cast<PairHybrid *>(force->pair_match("^hybrid",0));
  if (force->pair_match("^spin",0,0)) {        // only one Pair/Spin style
    pair = force->pair_match("^spin",0,0);
    if (hybrid == nullptr) npairs = 1;
    else npairs = hybrid->nstyles;
    npairspin = 1;
  } else if (force->pair_match("^spin",0,1)) { // more than one Pair/Spin style
    pair = force->pair_match("^spin",0,1);
    if (hybrid == nullptr) npairs = 1;
    else npairs = hybrid->nstyles;
    for (int i = 0; i<npairs; i++) {
      if (force->pair_match("^spin",0,i)) {
        npairspin ++;
      }
    }
  }

  // init length of vector of ptrs to Pair/Spin styles

  if (npairspin > 0) {
    spin_pairs = new PairSpin*[npairspin];
  }

  // loop 2: fill vector with ptrs to Pair/Spin styles

  int count1 = 0;
  if (npairspin == 1) {
    count1 = 1;
    spin_pairs[0] = dynamic_cast<PairSpin *>(force->pair_match("^spin",0,0));
  } else if (npairspin > 1) {
    for (int i = 0; i<npairs; i++) {
      if (force->pair_match("^spin",0,i)) {
        spin_pairs[count1] = dynamic_cast<PairSpin *>(force->pair_match("^spin",0,i));
        count1++;
      }
    }
  }

  if (count1 != npairspin)
    error->all(FLERR,"Incorrect number of spin pair styles");

  // set pair/spin and long/spin flags

  if (npairspin >= 1) pair_spin_flag = 1;

  for (int i = 0; i<npairs; i++) {
    if (force->pair_match("spin/long",0,i)) {
      long_spin_flag = 1;
    }
  }

  // set ptrs for fix precession/spin styles

  // loop 1: obtain # of fix precession/spin styles

  int iforce;
  nprecspin = 0;
  for (iforce = 0; iforce < modify->nfix; iforce++) {
    if (utils::strmatch(modify->fix[iforce]->style,"^precession/spin")) {
      nprecspin++;
    }
  }

  // init length of vector of ptrs to precession/spin styles

  if (nprecspin > 0) {
    lockprecessionspin = new FixPrecessionSpin*[nprecspin];
  }

  // loop 2: fill vector with ptrs to precession/spin styles

  int count2 = 0;
  if (nprecspin > 0) {
    for (iforce = 0; iforce < modify->nfix; iforce++) {
      if (utils::strmatch(modify->fix[iforce]->style,"^precession/spin")) {
        precession_spin_flag = 1;
        lockprecessionspin[count2] = dynamic_cast<FixPrecessionSpin *>(modify->fix[iforce]);
        count2++;
      }
    }
  }

  if (count2 != nprecspin)
    error->all(FLERR,"Incorrect number of precession/spin fixes");

  // set ptrs for fix langevin/spin styles

  // loop 1: obtain # of fix langevin/spin styles

  nlangspin = 0;
  for (iforce = 0; iforce < modify->nfix; iforce++) {
    if (utils::strmatch(modify->fix[iforce]->style,"^langevin/spin")) {
      nlangspin++;
    }
  }

  // init length of vector of ptrs to langevin/spin styles

  if (nlangspin > 0) {
    locklangevinspin = new FixLangevinSpin*[nlangspin];
  }

  // loop 2: fill vector with ptrs to langevin/spin styles

  count2 = 0;
  if (nlangspin > 0) {
    for (iforce = 0; iforce < modify->nfix; iforce++) {
      if (utils::strmatch(modify->fix[iforce]->style,"^langevin/spin")) {
        maglangevin_flag = 1;
        locklangevinspin[count2] = dynamic_cast<FixLangevinSpin *>(modify->fix[iforce]);
        count2++;
      }
    }
  }

  if (count2 != nlangspin)
    error->all(FLERR,"Incorrect number of langevin/spin fixes");

  // ptrs FixSetForceSpin classes

  for (iforce = 0; iforce < modify->nfix; iforce++) {
    if (utils::strmatch(modify->fix[iforce]->style,"^setforce/spin")) {
      setforce_spin_flag = 1;
      locksetforcespin = dynamic_cast<FixSetForceSpin *>(modify->fix[iforce]);
    }
  }

  // setting the sector variables/lists

  nsectors = 0;
  memory->create(rsec,3,"nve/spin:rsec");

  // perform the sectoring operation

  if (sector_flag) sectoring();

  // init. size of stacking lists (sectoring)

  nlocal_max = atom->nlocal;
  memory->grow(stack_head,nsectors,"nve/spin:stack_head");
  memory->grow(stack_foot,nsectors,"nve/spin:stack_foot");
  memory->grow(backward_stacks,nlocal_max,"nve/spin:backward_stacks");
  memory->grow(forward_stacks,nlocal_max,"nve/spin:forward_stacks");
}

/* ---------------------------------------------------------------------- */

void FixNVESpin::initial_integrate(int /*vflag*/)
{
  double dtfm;
  double hbar = force->hplanck/MY_2PI;  // eV/(rad.THz)
  double fmi_pairs[3];
  double fmi_one_side[3];
  double energy_single, energy_diff_langevin, energy_diff_one_side, energy_no_langevin, energy_one_side_no_left, energy_one_side_with_left;


  double **sp = atom->sp;
  double **x = atom->x;
  double **v = atom->v;
  double **f = atom->f;
  double *rmass = atom->rmass;
  double *mass = atom->mass;
  int nlocal = atom->nlocal;
  if (igroup == atom->firstgroup) nlocal = atom->nfirst;
  int *type = atom->type;
  int *mask = atom->mask;
  int number_groups = lmp->group->ngroup;

  double *group_sums;
  group_sums = new double[group->ngroup];
  for (int i = 0; i < group->ngroup; i++) group_sums[i] = 0.0;

  fmi_pairs[0] = 0.0;
  fmi_pairs[1] = 0.0;
  fmi_pairs[2] = 0.0;
  fmi_one_side[0] = 0.0;
  fmi_one_side[1] = 0.0;
  fmi_one_side[2] = 0.0;
  energy_no_langevin = 0.0;
  energy_one_side_no_left = 0.0;
  energy_one_side_with_left = 0.0;
  energy_diff_one_side = 0.0;
  
  // update half v for all atoms

  if (lattice_flag) {
    for (int i = 0; i < nlocal; i++) {
      if (mask[i] & groupbit) {
        if (rmass) dtfm = dtf / rmass[i];
        else dtfm = dtf / mass[type[i]];
        v[i][0] += dtfm * f[i][0];
        v[i][1] += dtfm * f[i][1];
        v[i][2] += dtfm * f[i][2];
      }
    }
  }

  // update half s for all atoms

  if (sector_flag) {                            // sectoring seq. update
    for (int j = 0; j < nsectors; j++) {        // advance quarter s for nlocal
      comm->forward_comm();
      int i = stack_foot[j];
      while (i >= 0) {
        if (mask[i] & groupbit) {
          if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {   // check if the z value of atom i is above the interface.
            ComputeInteractionsSpin_one_side(i, fmi_one_side);   // compute fmi of spin i with no atoms on its left before spin evolution
            AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_no_left); // compute one side energy of spin i after evolving with NO atoms on its left
          }
          ComputeInteractionsSpin(i, fmi_pairs);   // compute and store fmi. compute fmi_pairs
          AdvanceSingleSpin_no_langevin(i, fmi_pairs, &energy_no_langevin);  // compute energy of spin i after evolving with no langevin/spin thermostat
          AdvanceSingleSpin(i);  // spin evolution with all magnetic forces.
          energy_single = (sp[i][0]*fmi_pairs[0])+(sp[i][1]*fmi_pairs[1])+(sp[i][2]*fmi_pairs[2]);
          energy_diff_langevin = (energy_single-energy_no_langevin)*hbar;
          if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {  // calculate energy_one_side_no_left after normal spin evolution
            ComputeInteractionsSpin_one_side(i, fmi_one_side);  // compute fmi of spin i with no atoms on its left after spin evolution 
            AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_with_left);  // compute one side energy of spin i after evolving with atoms on its left
            energy_diff_one_side += (energy_one_side_with_left - energy_one_side_no_left)*hbar;
           // std::cout << "Atom  " << i << "  energy_single  " << energy_single << "  energy_one_side_no_left  " << energy_one_side_no_left << " energy_diff_one_side " << energy_diff_one_side << std::endl;
          }
          for (int j = 0; j < group->ngroup; j++) {
            int groupjbit = group->bitmask[j];
            if (mask[i] & groupjbit) {
              group_sums[j] += energy_diff_langevin;
            }
          }
          i = forward_stacks[i];
        }
      }
    }
    for (int j = nsectors-1; j >= 0; j--) {     // advance quarter s for nlocal
      comm->forward_comm();
      int i = stack_head[j];
      while (i >= 0) {
        if (mask[i] & groupbit) {
          if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {   // check if the z value of atom i is above the interface.
            ComputeInteractionsSpin_one_side(i, fmi_one_side);
            AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_no_left);
          }
          ComputeInteractionsSpin(i, fmi_pairs);
          AdvanceSingleSpin_no_langevin(i, fmi_pairs, &energy_no_langevin);
          AdvanceSingleSpin(i);
          energy_single = (sp[i][0]*fmi_pairs[0])+(sp[i][1]*fmi_pairs[1])+(sp[i][2]*fmi_pairs[2]);
          energy_diff_langevin = (energy_single-energy_no_langevin)*hbar;
          if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {
            ComputeInteractionsSpin_one_side(i, fmi_one_side);  // compute fmi of spin i with no atoms on its left after spin evolution 
            AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_with_left);  // compute one side energy of spin i after evolving with atoms on its left
            energy_diff_one_side += (energy_one_side_with_left - energy_one_side_no_left)*hbar;
          //  std::cout << "Atom  " << i << "  energy_single  " << energy_single << "  energy_one_side_no_left  " << energy_one_side_no_left << " energy_diff_one_side " << energy_diff_one_side << std::endl;
          }
          for (int j = 0; j < group->ngroup; j++) {
            int groupjbit = group->bitmask[j];
            if (mask[i] & groupjbit) {
              group_sums[j] += energy_diff_langevin;
            }
          }
          i = backward_stacks[i];
        }
      }
    }
  } else if (sector_flag == 0) {                // serial seq. update
    comm->forward_comm();                       // comm. positions of ghost atoms
    for (int i = 0; i < nlocal; i++) {           // advance quarter s for nlocal
      if (mask[i] & groupbit) {
        if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {   // check if the z value of atom i is above the interface.
          ComputeInteractionsSpin_one_side(i, fmi_one_side);
          AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_no_left);
        }
        ComputeInteractionsSpin(i, fmi_pairs);
        AdvanceSingleSpin_no_langevin(i, fmi_pairs, &energy_no_langevin);
        AdvanceSingleSpin(i);
        energy_single = (sp[i][0]*fmi_pairs[0])+(sp[i][1]*fmi_pairs[1])+(sp[i][2]*fmi_pairs[2]);
        energy_diff_langevin = (energy_single-energy_no_langevin)*hbar;
        if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {
          ComputeInteractionsSpin_one_side(i, fmi_one_side);  // compute fmi of spin i with no atoms on its left after spin evolution 
          AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_with_left);  // compute one side energy of spin i after evolving with atoms on its left
          energy_diff_one_side += (energy_one_side_with_left - energy_one_side_no_left)*hbar;
        //  std::cout << "Atom  " << i << "  energy_single  " << energy_single << "  energy_one_side_no_left  " << energy_one_side_no_left << " energy_diff_one_side " << energy_diff_one_side << std::endl;
        }
        for (int j = 0; j < group->ngroup; j++) {
          int groupjbit = group->bitmask[j];
          if (mask[i] & groupjbit) {
            group_sums[j] += energy_diff_langevin;
          }
        }
      }
    }
    for (int i = nlocal-1; i >= 0; i--) {        // advance quarter s for nlocal
      if (mask[i] & groupbit) {
        if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {   // check if the z value of atom i is above the interface.
          ComputeInteractionsSpin_one_side(i, fmi_one_side);
          AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_no_left);
        }
        ComputeInteractionsSpin(i, fmi_pairs);
        AdvanceSingleSpin_no_langevin(i, fmi_pairs, &energy_no_langevin);
        AdvanceSingleSpin(i);
        energy_single = (sp[i][0]*fmi_pairs[0])+(sp[i][1]*fmi_pairs[1])+(sp[i][2]*fmi_pairs[2]);
        energy_diff_langevin = (energy_single-energy_no_langevin)*hbar;
        if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {
          ComputeInteractionsSpin_one_side(i, fmi_one_side);  // compute fmi of spin i with no atoms on its left after spin evolution 
          AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_with_left);  // compute one side energy of spin i after evolving with atoms on its left
          energy_diff_one_side += (energy_one_side_with_left - energy_one_side_no_left)*hbar;
         // std::cout << "Atom  " << i << "  energy_single  " << energy_single << "  energy_one_side_no_left  " << energy_one_side_no_left << " energy_diff_one_side " << energy_diff_one_side << std::endl;
        }
        for (int j = 0; j < group->ngroup; j++) {
          int groupjbit = group->bitmask[j];
          if (mask[i] & groupjbit) {
            group_sums[j] += energy_diff_langevin;
          }
        }
      }
    }
  } else error->all(FLERR,"Illegal fix nve/spin command");

  // update x for all particles

  if (lattice_flag) {
    for (int i = 0; i < nlocal; i++) {
      if (mask[i] & groupbit) {
        x[i][0] += dtv * v[i][0];
        x[i][1] += dtv * v[i][1];
        x[i][2] += dtv * v[i][2];
      }
    }
  }

  // update half s for all particles

  if (sector_flag) {                            // sectoring seq. update
    for (int j = 0; j < nsectors; j++) {        // advance quarter s for nlocal
      comm->forward_comm();
      int i = stack_foot[j];
      while (i >= 0) {
        if (mask[i] & groupbit) {
          if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {   // check if the z value of atom i is above the interface.
            ComputeInteractionsSpin_one_side(i, fmi_one_side);
            AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_no_left);
          }
          ComputeInteractionsSpin(i, fmi_pairs);
          AdvanceSingleSpin_no_langevin(i, fmi_pairs, &energy_no_langevin);
          AdvanceSingleSpin(i);
          energy_single = (sp[i][0]*fmi_pairs[0])+(sp[i][1]*fmi_pairs[1])+(sp[i][2]*fmi_pairs[2]);
          energy_diff_langevin = (energy_single-energy_no_langevin)*hbar;
          if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {
            ComputeInteractionsSpin_one_side(i, fmi_one_side);  // compute fmi of spin i with no atoms on its left after spin evolution 
            AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_with_left);  // compute one side energy of spin i after evolving with atoms on its left
            energy_diff_one_side += (energy_one_side_with_left - energy_one_side_no_left)*hbar;
          //  std::cout << "Atom  " << i << "  energy_single  " << energy_single << "  energy_one_side_no_left  " << energy_one_side_no_left << " energy_diff_one_side " << energy_diff_one_side << std::endl;
          }
          for (int j = 0; j < group->ngroup; j++) {
            int groupjbit = group->bitmask[j];
            if (mask[i] & groupjbit) {
              group_sums[j] += energy_diff_langevin;
            }
          }
          i = forward_stacks[i];
        }
      }
    }
    for (int j = nsectors-1; j >= 0; j--) {     // advance quarter s for nlocal
      comm->forward_comm();
      int i = stack_head[j];
      while (i >= 0) {
        if (mask[i] & groupbit) {
          if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {   // check if the z value of atom i is above the interface.
            ComputeInteractionsSpin_one_side(i, fmi_one_side);
            AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_no_left);
          }
          ComputeInteractionsSpin(i, fmi_pairs);
          AdvanceSingleSpin_no_langevin(i, fmi_pairs, &energy_no_langevin);
          AdvanceSingleSpin(i);
          energy_single = (sp[i][0]*fmi_pairs[0])+(sp[i][1]*fmi_pairs[1])+(sp[i][2]*fmi_pairs[2]);
          energy_diff_langevin = (energy_single-energy_no_langevin)*hbar;
          if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {
            ComputeInteractionsSpin_one_side(i, fmi_one_side);  // compute fmi of spin i with no atoms on its left after spin evolution 
            AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_with_left);  // compute one side energy of spin i after evolving with atoms on its left
            energy_diff_one_side += (energy_one_side_with_left - energy_one_side_no_left)*hbar;
          //  std::cout << "Atom  " << i << "  energy_single  " << energy_single << "  energy_one_side_no_left  " << energy_one_side_no_left << " energy_diff_one_side " << energy_diff_one_side << std::endl;
          }
          for (int j = 0; j < group->ngroup; j++) {
            int groupjbit = group->bitmask[j];
            if (mask[i] & groupjbit) {
              group_sums[j] += energy_diff_langevin;
            }
          }
          i = backward_stacks[i];
        }
      }
    }
  } else {                                      // serial seq. update
    comm->forward_comm();                       // comm. positions of ghost atoms
    for (int i = 0; i < nlocal; i++) {          // advance quarter s for nlocal-1
      if (mask[i] & groupbit) {
        if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {   // check if the z value of atom i is above the interface.
          ComputeInteractionsSpin_one_side(i, fmi_one_side);
          AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_no_left);
        }
        ComputeInteractionsSpin(i, fmi_pairs);
        AdvanceSingleSpin_no_langevin(i, fmi_pairs, &energy_no_langevin);
        AdvanceSingleSpin(i);
        energy_single = (sp[i][0]*fmi_pairs[0])+(sp[i][1]*fmi_pairs[1])+(sp[i][2]*fmi_pairs[2]);
        energy_diff_langevin = (energy_single-energy_no_langevin)*hbar;
        if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {
          ComputeInteractionsSpin_one_side(i, fmi_one_side);  // compute fmi of spin i with no atoms on its left after spin evolution 
          AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_with_left);  // compute one side energy of spin i after evolving with atoms on its left
          energy_diff_one_side += (energy_one_side_with_left - energy_one_side_no_left)*hbar;
        //  std::cout << "Atom  " << i << "  energy_single  " << energy_single << "  energy_one_side_no_left  " << energy_one_side_no_left << " energy_diff_one_side " << energy_diff_one_side << std::endl;
        }
        for (int j = 0; j < group->ngroup; j++) {
          int groupjbit = group->bitmask[j];
          if (mask[i] & groupjbit) {
            group_sums[j] += energy_diff_langevin;
          }
        }
      }
    }
    for (int i = nlocal-1; i >= 0; i--) {       // advance quarter s for nlocal-1
      if (mask[i] & groupbit) {
        if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {   // check if the z value of atom i is above the interface.
          ComputeInteractionsSpin_one_side(i, fmi_one_side);
          AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_no_left);
        }
        ComputeInteractionsSpin(i, fmi_pairs);
        AdvanceSingleSpin_no_langevin(i, fmi_pairs, &energy_no_langevin);
        AdvanceSingleSpin(i);
        energy_single = (sp[i][0]*fmi_pairs[0])+(sp[i][1]*fmi_pairs[1])+(sp[i][2]*fmi_pairs[2]);
        energy_diff_langevin = (energy_single-energy_no_langevin)*hbar;
        if (x[i][2] > -0.1*2.8465 && x[i][2] < 2.1*2.8465) {
          ComputeInteractionsSpin_one_side(i, fmi_one_side);  // compute fmi of spin i with no atoms on its left after spin evolution 
          AdvanceSingleSpin_one_side(i, fmi_one_side, &energy_one_side_with_left);  // compute one side energy of spin i after evolving with atoms on its left
          energy_diff_one_side += (energy_one_side_with_left - energy_one_side_no_left)*hbar;
        //  std::cout << "Atom  " << i << "  energy_single  " << energy_single << "  energy_one_side_no_left  " << energy_one_side_no_left << " energy_diff_one_side " << energy_diff_one_side << std::endl;
        }
        for (int j = 0; j < group->ngroup; j++) {
          int groupjbit = group->bitmask[j];
          if (mask[i] & groupjbit) {
            group_sums[j] += energy_diff_langevin;
          }
        }
      }
    }
  }

  if (comm->me == 0) {                  // output the energy added by langevin/spin thermostats
    FILE *fp1 = fopen("group_sums.txt", "a");
    fprintf(fp1, "%d", update->ntimestep);
    for (int i = 0; i < group->ngroup; i++) {
      fprintf(fp1, " %s: %g", group->names[i], group_sums[i]);
    }
    fprintf(fp1, "\n");
    fclose(fp1);
  }

  if (comm->me == 0) {                  // output energy_diff_one_side
    FILE *fp2 = fopen("energy_interface15.txt", "a");
    fprintf(fp2, "%d", update->ntimestep);
    fprintf(fp2, " %g", energy_diff_one_side);
    fprintf(fp2, "\n");
    fclose(fp2);
  }
}

/* ----------------------------------------------------------------------
   setup pre_neighbor()
---------------------------------------------------------------------- */

void FixNVESpin::setup_pre_neighbor()
{
  pre_neighbor();
}

/* ----------------------------------------------------------------------
   store in two linked lists the advance order of the spins (sectoring)
---------------------------------------------------------------------- */

void FixNVESpin::pre_neighbor()
{
  double **x = atom->x;
  int nlocal = atom->nlocal;

  if (nlocal_max < nlocal) {                    // grow linked lists if necessary
    nlocal_max = nlocal;
    memory->grow(backward_stacks,nlocal_max,"nve/spin:backward_stacks");
    memory->grow(forward_stacks,nlocal_max,"nve/spin:forward_stacks");
  }

  for (int j = 0; j < nsectors; j++) {
    stack_head[j] = -1;
    stack_foot[j] = -1;
  }

  int nseci;
  for (int j = 0; j < nsectors; j++) {          // stacking backward order
    for (int i = 0; i < nlocal; i++) {
      nseci = coords2sector(x[i]);
      if (j != nseci) continue;
      backward_stacks[i] = stack_head[j];
      stack_head[j] = i;
    }
  }
  for (int j = nsectors-1; j >= 0; j--) {       // stacking forward order
    for (int i = nlocal-1; i >= 0; i--) {
      nseci = coords2sector(x[i]);
      if (j != nseci) continue;
      forward_stacks[i] = stack_foot[j];
      stack_foot[j] = i;
    }
  }

}

/* ----------------------------------------------------------------------
   compute the magnetic torque for the spin ii
---------------------------------------------------------------------- */

void FixNVESpin::ComputeInteractionsSpin(int i, double fmi_pairs[3])
{
  double spi[3], fmi[3];

  double **sp = atom->sp;
  double **fm = atom->fm;

  // force computation for spin i

  spi[0] = sp[i][0];
  spi[1] = sp[i][1];
  spi[2] = sp[i][2];

  fmi[0] = fmi[1] = fmi[2] = 0.0;

  // update magnetic pair interactions

  if (pair_spin_flag) {
    for (int k = 0; k < npairspin; k++) {
      spin_pairs[k]->compute_single_pair(i,fmi);
    }
  }
  fmi_pairs[0] = fmi[0];
  fmi_pairs[1] = fmi[1];
  fmi_pairs[2] = fmi[2];

  // update magnetic precession interactions

  if (precession_spin_flag) {
    for (int k = 0; k < nprecspin; k++) {
      lockprecessionspin[k]->compute_single_precession(i,spi,fmi);
    }
  }

  // update langevin damping and random force

  if (maglangevin_flag) {               // mag. langevin
    for (int k = 0; k < nlangspin; k++) {
      locklangevinspin[k]->compute_single_langevin(i,spi,fmi);
    }
  }

  // update setforce of magnetic interactions

  if (setforce_spin_flag) {
    locksetforcespin->single_setforce_spin(i,fmi);
  }

  // replace the magnetic force fm[i] by its new value fmi

  fm[i][0] = fmi[0];
  fm[i][1] = fmi[1];
  fm[i][2] = fmi[2];
}

/* ----------------------------------------------------------------------
  ***added by Weidi***
   Compute the pair interaction fmi from one side
   do not change fm[i]
---------------------------------------------------------------------- */
void FixNVESpin::ComputeInteractionsSpin_one_side(int i, double fmi_one_side[3])
{
  double spi[3], fmi[3];

  double **sp = atom->sp;
  double **fm = atom->fm;

  // force computation for spin i

  spi[0] = sp[i][0];
  spi[1] = sp[i][1];
  spi[2] = sp[i][2];

  fmi[0] = fmi[1] = fmi[2] = 0.0;

  // update magnetic pair interactions

  if (pair_spin_flag) {
    for (int k = 0; k < npairspin; k++) {
      spin_pairs[k]->compute_single_pair_one_side(i,fmi,-0.1*2.8465);
    }
  }

  // update magnetic precession interactions

  if (precession_spin_flag) {
    for (int k = 0; k < nprecspin; k++) {
      lockprecessionspin[k]->compute_single_precession(i,spi,fmi);
    }
  }

  // update langevin damping and random force

  if (maglangevin_flag) {               // mag. langevin
    for (int k = 0; k < nlangspin; k++) {
      locklangevinspin[k]->compute_single_langevin(i,spi,fmi);
    }
  }

  // update setforce of magnetic interactions

  if (setforce_spin_flag) {
    locksetforcespin->single_setforce_spin(i,fmi);
  }

  // replace the magnetic force fmi_one_side by its new value fmi

  fmi_one_side[0] = fmi[0];
  fmi_one_side[1] = fmi[1];
  fmi_one_side[2] = fmi[2];
}

/* ----------------------------------------------------------------------
   divide each domain into 8 sectors
---------------------------------------------------------------------- */

void FixNVESpin::sectoring()
{
  int sec[3];
  double sublo[3],subhi[3];

  if (domain->triclinic == 1){
     double* sublotmp = domain->sublo_lamda;
     double* subhitmp = domain->subhi_lamda;
     for (int dim = 0 ; dim < 3 ; dim++) {
       sublo[dim]=sublotmp[dim]*domain->boxhi[dim];
       subhi[dim]=subhitmp[dim]*domain->boxhi[dim];
     }
  }

  else {
     double* sublotmp = domain->sublo;
     double* subhitmp = domain->subhi;
     for (int dim = 0 ; dim < 3 ; dim++) {
       sublo[dim]=sublotmp[dim];
       subhi[dim]=subhitmp[dim];
     }
  }

  const double rsx = subhi[0] - sublo[0];
  const double rsy = subhi[1] - sublo[1];
  const double rsz = subhi[2] - sublo[2];

  // extract larger cutoff from PairSpin styles

  double rv, cutoff;
  rv = cutoff = 0.0;
  int dim = 0;
  for (int i = 0; i < npairspin ; i++) {
    cutoff = *((double *) spin_pairs[i]->extract("cut",dim));
    rv = MAX(rv,cutoff);
  }

  if (rv == 0.0)
   error->all(FLERR,"Illegal sectoring operation");

  double rax = rsx/rv;
  double ray = rsy/rv;
  double raz = rsz/rv;

  sec[0] = 1;
  sec[1] = 1;
  sec[2] = 1;
  if (rax >= 2.0) sec[0] = 2;
  if (ray >= 2.0) sec[1] = 2;
  if (raz >= 2.0) sec[2] = 2;

  nsectors = sec[0]*sec[1]*sec[2];

  if (sector_flag == 1 && nsectors != 8)
    error->all(FLERR,"Illegal sectoring operation");

  rsec[0] = rsx;
  rsec[1] = rsy;
  rsec[2] = rsz;
  if (sec[0] == 2) rsec[0] = rsx/2.0;
  if (sec[1] == 2) rsec[1] = rsy/2.0;
  if (sec[2] == 2) rsec[2] = rsz/2.0;

}

/* ----------------------------------------------------------------------
   define sector for an atom at a position x[i]
---------------------------------------------------------------------- */

int FixNVESpin::coords2sector(double *x)
{
  int nseci;
  int seci[3];
  double sublo[3];
  double* sublotmp = domain->sublo;
  for (int dim = 0 ; dim<3 ; dim++) {
    sublo[dim]=sublotmp[dim];
  }

  seci[0] = x[0] > (sublo[0] + rsec[0]);
  seci[1] = x[1] > (sublo[1] + rsec[1]);
  seci[2] = x[2] > (sublo[2] + rsec[2]);

  nseci = (seci[0] + 2*seci[1] + 4*seci[2]);

  return nseci;
}

/* ----------------------------------------------------------------------
   advance the spin i of a timestep dts
---------------------------------------------------------------------- */

void FixNVESpin::AdvanceSingleSpin(int i)
{
  int j=0;
  int *sametag = atom->sametag;
  double **sp = atom->sp;
  double **fm = atom->fm;
  double fm2,energy,dts2;
  double cp[3],g[3];

  cp[0] = cp[1] = cp[2] = 0.0;
  g[0] = g[1] = g[2] = 0.0;
  fm2 = (fm[i][0]*fm[i][0])+(fm[i][1]*fm[i][1])+(fm[i][2]*fm[i][2]);
  energy = (sp[i][0]*fm[i][0])+(sp[i][1]*fm[i][1])+(sp[i][2]*fm[i][2]);
  dts2 = dts*dts;

  cp[0] = fm[i][1]*sp[i][2] - fm[i][2]*sp[i][1];
  cp[1] = fm[i][2]*sp[i][0] - fm[i][0]*sp[i][2];
  cp[2] = fm[i][0]*sp[i][1] - fm[i][1]*sp[i][0];

  g[0] = sp[i][0] + cp[0]*dts;
  g[1] = sp[i][1] + cp[1]*dts;
  g[2] = sp[i][2] + cp[2]*dts;

  g[0] += (fm[i][0]*energy - 0.5*sp[i][0]*fm2)*0.5*dts2;
  g[1] += (fm[i][1]*energy - 0.5*sp[i][1]*fm2)*0.5*dts2;
  g[2] += (fm[i][2]*energy - 0.5*sp[i][2]*fm2)*0.5*dts2;

  g[0] /= (1.0 + 0.25*fm2*dts2);
  g[1] /= (1.0 + 0.25*fm2*dts2);
  g[2] /= (1.0 + 0.25*fm2*dts2);

  sp[i][0] = g[0];
  sp[i][1] = g[1];
  sp[i][2] = g[2];

  // renormalization (check if necessary)

  // msq = g[0]*g[0] + g[1]*g[1] + g[2]*g[2];
  // scale = 1.0/sqrt(msq);
  // sp[i][0] *= scale;
  // sp[i][1] *= scale;
  // sp[i][2] *= scale;

  // comm. sp[i] to atoms with same tag (for serial algo)

  if (sector_flag == 0) {
    if (sametag[i] >= 0) {
      j = sametag[i];
      while (j >= 0) {
        sp[j][0] = sp[i][0];
        sp[j][1] = sp[i][1];
        sp[j][2] = sp[i][2];
        j = sametag[j];
      }
    }
  }

}
/* ----------------------------------------------------------------------
  ***added by Weidi***
   advance the spin i of a timestep dts, with no langevin/spin thermostats
   do not change sp[i]
---------------------------------------------------------------------- */
void FixNVESpin::AdvanceSingleSpin_no_langevin(int i, double fmi_pairs[3], double*energy_no_langevin)
{
  int j=0;
  int *sametag = atom->sametag;
  double **sp = atom->sp;
  //double **fm = atom->fm;
  double fm2,energy,dts2;
  double cp[3],g[3];

  cp[0] = cp[1] = cp[2] = 0.0;
  g[0] = g[1] = g[2] = 0.0;
  fm2 = (fmi_pairs[0]*fmi_pairs[0])+(fmi_pairs[1]*fmi_pairs[1])+(fmi_pairs[2]*fmi_pairs[2]);
  energy = (sp[i][0]*fmi_pairs[0])+(sp[i][1]*fmi_pairs[1])+(sp[i][2]*fmi_pairs[2]);
  dts2 = dts*dts;

  cp[0] = fmi_pairs[1]*sp[i][2] - fmi_pairs[2]*sp[i][1];
  cp[1] = fmi_pairs[2]*sp[i][0] - fmi_pairs[0]*sp[i][2];
  cp[2] = fmi_pairs[0]*sp[i][1] - fmi_pairs[1]*sp[i][0];

  g[0] = sp[i][0] + cp[0]*dts;
  g[1] = sp[i][1] + cp[1]*dts;
  g[2] = sp[i][2] + cp[2]*dts;

  g[0] += (fmi_pairs[0]*energy - 0.5*sp[i][0]*fm2)*0.5*dts2;
  g[1] += (fmi_pairs[1]*energy - 0.5*sp[i][1]*fm2)*0.5*dts2;
  g[2] += (fmi_pairs[2]*energy - 0.5*sp[i][2]*fm2)*0.5*dts2;

  g[0] /= (1.0 + 0.25*fm2*dts2);
  g[1] /= (1.0 + 0.25*fm2*dts2);
  g[2] /= (1.0 + 0.25*fm2*dts2);

  *energy_no_langevin = (g[0]*fmi_pairs[0])+(g[1]*fmi_pairs[1])+(g[2]*fmi_pairs[2]);

}

/* ----------------------------------------------------------------------
  ***added by Weidi***
   advance the spin i of a timestep dts, with no langevin/spin thermostats
   do not change sp[i]
---------------------------------------------------------------------- */
void FixNVESpin::AdvanceSingleSpin_one_side(int i, double fmi_one_side[3], double*energy_one_side_no_left)
{
  int j=0;
  int *sametag = atom->sametag;
  double **sp = atom->sp;
  //double **fm = atom->fm;
  double fm2,energy,dts2;
  double cp[3],g[3];

  cp[0] = cp[1] = cp[2] = 0.0;
  g[0] = g[1] = g[2] = 0.0;
  fm2 = (fmi_one_side[0]*fmi_one_side[0])+(fmi_one_side[1]*fmi_one_side[1])+(fmi_one_side[2]*fmi_one_side[2]);
  energy = (sp[i][0]*fmi_one_side[0])+(sp[i][1]*fmi_one_side[1])+(sp[i][2]*fmi_one_side[2]);
  dts2 = dts*dts;

  cp[0] = fmi_one_side[1]*sp[i][2] - fmi_one_side[2]*sp[i][1];
  cp[1] = fmi_one_side[2]*sp[i][0] - fmi_one_side[0]*sp[i][2];
  cp[2] = fmi_one_side[0]*sp[i][1] - fmi_one_side[1]*sp[i][0];

  g[0] = sp[i][0] + cp[0]*dts;
  g[1] = sp[i][1] + cp[1]*dts;
  g[2] = sp[i][2] + cp[2]*dts;

  g[0] += (fmi_one_side[0]*energy - 0.5*sp[i][0]*fm2)*0.5*dts2;
  g[1] += (fmi_one_side[1]*energy - 0.5*sp[i][1]*fm2)*0.5*dts2;
  g[2] += (fmi_one_side[2]*energy - 0.5*sp[i][2]*fm2)*0.5*dts2;

  g[0] /= (1.0 + 0.25*fm2*dts2);
  g[1] /= (1.0 + 0.25*fm2*dts2);
  g[2] /= (1.0 + 0.25*fm2*dts2);

  *energy_one_side_no_left = (g[0]*fmi_one_side[0])+(g[1]*fmi_one_side[1])+(g[2]*fmi_one_side[2]);

}


/* ---------------------------------------------------------------------- */

void FixNVESpin::final_integrate()
{
  double dtfm;

  double **v = atom->v;
  double **f = atom->f;
  double *rmass = atom->rmass;
  double *mass = atom->mass;
  int nlocal = atom->nlocal;
  if (igroup == atom->firstgroup) nlocal = atom->nfirst;
  int *type = atom->type;
  int *mask = atom->mask;

  // update half v for all particles

  if (lattice_flag) {
    for (int i = 0; i < nlocal; i++) {
      if (mask[i] & groupbit) {
        if (rmass) dtfm = dtf / rmass[i];
        else dtfm = dtf / mass[type[i]];
        v[i][0] += dtfm * f[i][0];
        v[i][1] += dtfm * f[i][1];
        v[i][2] += dtfm * f[i][2];
      }
    }
  }
}
