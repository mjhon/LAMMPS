/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   This software is distributed under the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Axel Kohlmeyer (Temple U)
------------------------------------------------------------------------- */

#include "math.h"
#include "pair_coul_long_omp.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "neighbor.h"
#include "neigh_list.h"

using namespace LAMMPS_NS;

#define EWALD_F   1.12837917
#define EWALD_P   0.3275911
#define A1        0.254829592
#define A2       -0.284496736
#define A3        1.421413741
#define A4       -1.453152027
#define A5        1.061405429

/* ---------------------------------------------------------------------- */

PairCoulLongOMP::PairCoulLongOMP(LAMMPS *lmp) :
  PairCoulLong(lmp), ThrOMP(lmp, PAIR)
{
  respa_enable = 0;
}

/* ---------------------------------------------------------------------- */

void PairCoulLongOMP::compute(int eflag, int vflag)
{
  if (eflag || vflag) {
    ev_setup(eflag,vflag);
    ev_setup_thr(this);
  } else evflag = vflag_fdotr = 0;

  const int nall = atom->nlocal + atom->nghost;
  const int nthreads = comm->nthreads;
  const int inum = list->inum;

#if defined(_OPENMP)
#pragma omp parallel default(shared)
#endif
  {
    int ifrom, ito, tid;
    double **f;

    f = loop_setup_thr(atom->f, ifrom, ito, tid, inum, nall, nthreads);

    if (evflag) {
      if (eflag) {
	if (force->newton_pair) eval<1,1,1>(f, ifrom, ito, tid);
	else eval<1,1,0>(f, ifrom, ito, tid);
      } else {
	if (force->newton_pair) eval<1,0,1>(f, ifrom, ito, tid);
	else eval<1,0,0>(f, ifrom, ito, tid);
      }
    } else {
      if (force->newton_pair) eval<0,0,1>(f, ifrom, ito, tid);
      else eval<0,0,0>(f, ifrom, ito, tid);
    }

    // reduce per thread forces into global force array.
    data_reduce_thr(&(atom->f[0][0]), nall, nthreads, 3, tid);
  } // end of omp parallel region

  // reduce per thread energy and virial, if requested.
  if (evflag) ev_reduce_thr(this);
  if (vflag_fdotr) virial_fdotr_compute();
}

/* ---------------------------------------------------------------------- */

template <int EVFLAG, int EFLAG, int NEWTON_PAIR>
void PairCoulLongOMP::eval(double **f, int iifrom, int iito, int tid)
{
  int i,j,ii,jj,jnum,itable,itype,jtype;
  double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,ecoul,fpair;
  double fraction,table;
  double r,r2inv,rsq,forcecoul,factor_coul;
  double grij,expm2,prefactor,t,erfc;
  int *ilist,*jlist,*numneigh,**firstneigh;

  ecoul = 0.0;

  double **x = atom->x;
  double *q = atom->q;
  int *type = atom->type;
  int nlocal = atom->nlocal;
  double *special_coul = force->special_coul;
  double qqrd2e = force->qqrd2e;
  double fxtmp,fytmp,fztmp;

  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  // loop over neighbors of my atoms

  for (ii = iifrom; ii < iito; ++ii) {

    i = ilist[ii];
    qtmp = q[i];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    itype = type[i];
    jlist = firstneigh[i];
    jnum = numneigh[i];
    fxtmp=fytmp=fztmp=0.0;

    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      factor_coul = special_coul[sbmask(j)];
      j &= NEIGHMASK;

      delx = xtmp - x[j][0];
      dely = ytmp - x[j][1];
      delz = ztmp - x[j][2];
      rsq = delx*delx + dely*dely + delz*delz;
      jtype = type[j];

      if (rsq < cut_coulsq) {
	r2inv = 1.0/rsq;
	if (!ncoultablebits || rsq <= tabinnersq) {
	  r = sqrt(rsq);
	  grij = g_ewald * r;
	  expm2 = exp(-grij*grij);
	  t = 1.0 / (1.0 + EWALD_P*grij);
	  erfc = t * (A1+t*(A2+t*(A3+t*(A4+t*A5)))) * expm2;
	  prefactor = qqrd2e * scale[itype][jtype] * qtmp*q[j]/r;
	  forcecoul = prefactor * (erfc + EWALD_F*grij*expm2);
	  if (factor_coul < 1.0) forcecoul -= (1.0-factor_coul)*prefactor;
	} else {
	  union_int_float_t rsq_lookup;
	  rsq_lookup.f = rsq;
	  itable = rsq_lookup.i & ncoulmask;
	  itable >>= ncoulshiftbits;
	  fraction = (rsq_lookup.f - rtable[itable]) * drtable[itable];
	  table = ftable[itable] + fraction*dftable[itable];
	  forcecoul = scale[itype][jtype] * qtmp*q[j] * table;
	  if (factor_coul < 1.0) {
	    table = ctable[itable] + fraction*dctable[itable];
	    prefactor = scale[itype][jtype] * qtmp*q[j] * table;
	    forcecoul -= (1.0-factor_coul)*prefactor;
	  }
	}

	fpair = forcecoul * r2inv;

	fxtmp += delx*fpair;
	fytmp += dely*fpair;
	fztmp += delz*fpair;
	if (NEWTON_PAIR || j < nlocal) {
	  f[j][0] -= delx*fpair;
	  f[j][1] -= dely*fpair;
	  f[j][2] -= delz*fpair;
	}

	if (EFLAG) {
	  if (!ncoultablebits || rsq <= tabinnersq)
	    ecoul = prefactor*erfc;
	  else {
	    table = etable[itable] + fraction*detable[itable];
	    ecoul = scale[itype][jtype] * qtmp*q[j] * table;
	  }
	  if (factor_coul < 1.0) ecoul -= (1.0-factor_coul)*prefactor;
	}

	if (EVFLAG) ev_tally_thr(this, i,j,nlocal,NEWTON_PAIR,
				 0.0,ecoul,fpair,delx,dely,delz,tid);
      }
    }
    f[i][0] += fxtmp;
    f[i][1] += fytmp;
    f[i][2] += fztmp;
  }
}

/* ---------------------------------------------------------------------- */

double PairCoulLongOMP::memory_usage()
{
  double bytes = memory_usage_thr();
  bytes += PairCoulLong::memory_usage();

  return bytes;
}