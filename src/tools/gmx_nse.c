/*
 *
 *                This source code is part of
 *
 *                 G   R   O   M   A   C   S
 *
 *          GROningen MAchine for Chemical Simulations
 *
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 *
 * For more info, check our website at http://www.gromacs.org
 *
 * And Hey:
 * Green Red Orange Magenta Azure Cyan Skyblue
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <string.h>
#include "smalloc.h"
#include "sysstuff.h"
#include "typedefs.h"
#include "macros.h"
#include "vec.h"
#include "pbc.h"
#include "xvgr.h"
#include "copyrite.h"
#include "futil.h"
#include "statutil.h"
#include "tpxio.h"
#include "index.h"
#include "gstat.h"
#include "matio.h"
#include "gmx_ana.h"
#include "nsfactor.h"
#include "gmx_omp.h"

#ifndef PATH_MAX
#if (defined WIN32 || defined _WIN32 || defined WIN64 || defined _WIN64)
#ifdef MAX_PATH
#define PATH_MAX MAX_PATH
#else
#define PATH_MAX 260
#endif
#endif
#endif

int gmx_nse(int argc,char *argv[])
{
    const char *desc[] = {
    "This is simple tool to compute Neutron Spin Echo (NSE) spectra using Debye formula",
        "Besides the trajectory, the topology is required to assign elements to each atom.",
        "[PAR]",
        "[TT]-sqt[TT] Computes NSE intensity curve for each q value",
        "[PAR]",
        "[TT]-startq[TT] Starting q value in nm",
        "[PAR]",
        "[TT]-endq[TT] Ending q value in nm",
        "[PAR]",
        "[TT]-qstep[TT] Stepping in q space",
        "[PAR]",
        "Note: This tools produces large number of sqt files (one file per needed q value)!"
    };
    static gmx_bool bPBC=TRUE;
    static gmx_bool bNSE=TRUE;
    static gmx_bool bNORM=FALSE;
    static real binwidth=0.2,grid=0.05; /* bins shouldnt be smaller then bond (~0.1nm) length */
    static real start_q=0.01, end_q=2.0, q_step=0.01;
    static real mcover=-1;
    static unsigned int  seed=0;
    static int  nthreads=-1;

    static const char *emode[]= { NULL, "direct", "mc", NULL };
    static const char *emethod[]={ NULL, "debye", "fft", NULL };

    gmx_neutron_atomic_structurefactors_t    *gnsf;
    gmx_nse_t              *gnse;

#define NPA asize(pa)

    t_pargs pa[] = {
        { "-bin", FALSE, etREAL, {&binwidth},
          "Binwidth (nm)" },
        { "-mode", FALSE, etENUM, {emode},
          "Mode for sans spectra calculation" },
        { "-mcover", FALSE, etREAL, {&mcover},
          "Number of iterations for Monte-Carlo run"},
        { "-method", FALSE, etENUM, {emethod},
          "[HIDDEN]Method for sans spectra calculation" },
        { "-pbc", FALSE, etBOOL, {&bPBC},
          "Use periodic boundary conditions for computing distances" },
        { "-grid", FALSE, etREAL, {&grid},
          "[HIDDEN]Grid spacing (in nm) for FFTs" },
        {"-startq", FALSE, etREAL, {&start_q},
          "Starting q (1/nm) "},
        {"-endq", FALSE, etREAL, {&end_q},
          "Ending q (1/nm)"},
        { "-qstep", FALSE, etREAL, {&q_step},
          "Stepping in q (1/nm)"},
        { "-seed",     FALSE, etINT,  {&seed},
          "Random seed for Monte-Carlo"},
#ifdef GMX_OPENMP
        { "-nt",    FALSE, etINT, {&nthreads},
          "Number of threads to start"},
#endif
    };
  FILE      *fp;
  const char *fnTPX,*fnTRX,*fnNDX,*fnDAT=NULL;
  t_trxstatus *status;
  t_topology *top=NULL;
  t_atom    *atom=NULL;
  t_atoms   *atoms=NULL;
  gmx_rmpbc_t  gpbc=NULL;
  gmx_bool  bTPX;
  gmx_bool  bFFT=FALSE, bDEBYE=FALSE;
  gmx_bool  bMC=FALSE;
  int        ePBC=-1;
  matrix     box;
  char       title[STRLEN];
  rvec       *x,*xf;
  int       natoms;
  int       nframes;
  int       nralloc=1;
  int       pairs;
  real      t;
  char       **grpname=NULL;
  atom_id    *index=NULL;
  int        isize;
  int         i,j,k;
  t_filenm    *fnmdup=NULL;
  char        *sqtf_base=NULL, *sqtf=NULL, hdr[40];
  char        *stqf_base=NULL, *stqf=NULL;
  char        *grtf_base=NULL, *grtf=NULL;
  const char  *sqtf_ext=NULL, *stqf_ext=NULL, *grtf_ext=NULL;
  gmx_radial_distribution_histogram_t   *grc=NULL;
  output_env_t oenv;

#define NFILE asize(fnm)

  t_filenm   fnm[] = {
      { efTPX,  "-s",         NULL,   ffREAD },
      { efTRX,  "-f",         NULL,   ffREAD },
      { efNDX,  NULL,         NULL,   ffOPTRD },
      { efDAT,  "-d",   "nsfactor",   ffOPTRD },
      { efXVG, "-grt",       "grt",   ffOPTWR },
      { efXVG, "-sqt",       "sqt",   ffWRITE },
      { efXVG, "-stq",       "stq",   ffOPTWR }
  };


  nthreads = gmx_omp_get_max_threads();

  CopyRight(stderr,argv[0]);
  parse_common_args(&argc,argv,PCA_CAN_TIME | PCA_TIME_UNIT | PCA_BE_NICE,
                    NFILE,fnm,asize(pa),pa,asize(desc),desc,0,NULL,&oenv);

  /* Check binwith and mcover */
  check_binwidth(binwidth);
  check_mcover(mcover);

  gmx_omp_set_num_threads(nthreads);

  /* Now try to parse opts for modes */
  switch(emethod[0][0]) {
  case 'd':
      bDEBYE=TRUE;
      switch(emode[0][0]) {
      case 'd':
          bMC=FALSE;
          break;
      case 'm':
          bMC=TRUE;
          break;
      default:
          break;
      }
      break;
  case 'f':
      bFFT=TRUE;
      break;
  default:
      break;
  }

  if (bDEBYE) {
      if (bMC) {
          fprintf(stderr,"Using Monte Carlo Debye method to calculate spectrum\n");
      } else {
          fprintf(stderr,"Using direct Debye method to calculate spectrum\n");
      }
  } else if (bFFT) {
      gmx_fatal(FARGS,"FFT method not implemented!");
  } else {
      gmx_fatal(FARGS,"Unknown combination for mode and method!");
  }

  /* Try to read files */
  fnDAT = ftp2fn(efDAT,NFILE,fnm);
  fnTPX = ftp2fn(efTPX,NFILE,fnm);
  fnTRX = ftp2fn(efTRX,NFILE,fnm);

  gnsf = gmx_neutronstructurefactors_init(fnDAT);
  fprintf(stderr,"Read %d atom names from %s with neutron scattering parameters\n\n",gnsf->nratoms,fnDAT);

  /* prepare filenames for sqt output */
  sqtf_ext = strrchr(opt2fn_null("-sqt",NFILE,fnm),'.');
  if (sqtf_ext == NULL)
      gmx_fatal(FARGS,"Output file name '%s' does not contain a '.'",opt2fn_null("-sqt",NFILE,fnm));
  sqtf_base = strdup(opt2fn_null("-sqt",NFILE,fnm));
  sqtf_base[sqtf_ext - opt2fn_null("-sqt",NFILE,fnm)] = '\0';

  /* prepare filenames for stq output if exist*/
  if(opt2fn_null("-stq",NFILE,fnm)) {
      stqf_ext = strrchr(opt2fn_null("-stq",NFILE,fnm),'.');
      if (stqf_ext == NULL)
          gmx_fatal(FARGS,"Output file name '%s' does not contain a '.'",opt2fn_null("-stq",NFILE,fnm));
      stqf_base = strdup(opt2fn_null("-stq",NFILE,fnm));
      stqf_base[stqf_ext - opt2fn_null("-stq",NFILE,fnm)] = '\0';
  }

  /* prepare filenames for grt output if exist*/
  if(opt2fn_null("-grt",NFILE,fnm)) {
      grtf_ext = strrchr(opt2fn_null("-grt",NFILE,fnm),'.');
      if (grtf_ext == NULL)
          gmx_fatal(FARGS,"Output file name '%s' does not contain a '.'",opt2fn_null("-grt",NFILE,fnm));
      grtf_base = strdup(opt2fn_null("-grt",NFILE,fnm));
      grtf_base[grtf_ext - opt2fn_null("-grt",NFILE,fnm)] = '\0';
  }

  snew(top,1);
  snew(gnse,1);
  snew(grpname,1);
  snew(index,1);

  bTPX=read_tps_conf(fnTPX,title,top,&ePBC,&x,NULL,box,TRUE);

  atoms=&(top->atoms);

  printf("\nPlease select group for SANS spectra calculation:\n");
  get_index(&(top->atoms),ftp2fn_null(efNDX,NFILE,fnm),1,&isize,&index,grpname);

  gnse->sans = gmx_sans_init(top,gnsf);

  /* Prepare reference frame */
  if (bPBC) {
      gpbc = gmx_rmpbc_init(&top->idef,ePBC,top->atoms.nr,box);
  }

  natoms=read_first_x(oenv,&status,fnTRX,&t,&xf,box);
  if (natoms != atoms->nr)
      fprintf(stderr,"\nWARNING: number of atoms in tpx (%d) and trajectory (%d) do not match\n",natoms,atoms->nr);
  /* realy do calc */
  nframes=0;
  snew(gnse->x,nralloc);
  snew(gnse->t,nralloc);
  gnse->t[0]=t;
  /* now we need to copy only selected group to gnse->x[0] */
  snew(gnse->x[0],isize);
  for(i=0;i<isize;i++) {
      copy_rvec(xf[index[i]],gnse->x[0][i]);
  }

  /* Read whole trajectory into memroy and allocate gnse structure */
  do {
      if (bPBC) {
          gmx_rmpbc(gpbc,atoms->nr,box,xf);
      }
      gnse->nrframes = nframes+1;
      if(nralloc<(nframes+1)) {
          nralloc++;
          srenew(gnse->gr,nralloc);
          srenew(gnse->sq,nralloc);
          srenew(gnse->x,nralloc);
      }
      snew(gnse->x[nframes],isize);
      for(i=0;i<isize;i++) {
          copy_rvec(xf[index[i]],gnse->x[nframes][i]);
      }
      nframes++;
  } while (read_next_x(oenv,status,&t,natoms,xf,box));
  close_trj(status);
  /* populate gnse->t structure with md times */
  srenew(gnse->t,gnse->nrframes);
  for(i=1;i<gnse->nrframes;i++) {
      gnse->t[i]= gnse->t[0] + i * (t - gnse->t[0])/(gnse->nrframes - 1);
  }

  /*
   * now try to populate avereged over time cross histograms t,t+dt
   * j = dt, frame = i
   */
  pairs = (int)floor(0.5*gnse->nrframes*(gnse->nrframes+1));
  snew(gnse->dt,gnse->nrframes);
  fprintf(stderr,"Total numer of pairs = %10d\n",pairs);

  for(i=0;i<gnse->nrframes;i++) {
      for(j=0;j+i<gnse->nrframes;j++) {
          snew(grc,1);
          snew(gnse->gr[i],1);
          grc = calc_radial_distribution_histogram(gnse->sans,gnse->x[j],gnse->x[j+i],box,index,isize,binwidth,bMC,bNORM,bNSE,mcover,seed);
          /* now we need to summ up gr's */
          gnse->gr[i]->binwidth = grc->binwidth;
          gnse->gr[i]->grn = grc->grn;
          snew(gnse->gr[i]->gr,grc->grn);
          snew(gnse->gr[i]->r,grc->grn);
          for(k=0;k<grc->grn;k++) {
              gnse->gr[i]->gr[k] += grc->gr[k];
              gnse->gr[i]->r[k] = grc->r[k];
          }
          sfree(grc);
          pairs--;
          gnse->dt[i] = gnse->t[j+i]-gnse->t[j];
          fprintf(stderr,"\rdt = %10.2f pairs left to compute %10d",gnse->dt[i],pairs);
      }
      normalize_probability(gnse->gr[i]->grn,gnse->gr[i]->gr);
      gnse->sq[i] = convert_histogram_to_intensity_curve(gnse->gr[i],start_q,end_q,q_step);
      if(opt2fn_null("-stq",NFILE,fnm)) {
          snew(stqf,PATH_MAX);
          sprintf(stqf,"%s_%010.2f%s",stqf_base,gnse->dt[i],stqf_ext);
          sprintf(hdr,"S(q,dt), dt = %10.2f",gnse->dt[i]);
          fp = xvgropen(stqf,hdr,"q, nm^-1","S(q,t) arb.u.",oenv);
          for(j=0;j<gnse->sq[i]->qn;j++) {
              fprintf(fp,"%10.6lf%10.6lf\n",gnse->sq[i]->q[j],gnse->sq[i]->s[j]);
          }
          fclose(fp);
          sfree(stqf);
      }
      if(opt2fn_null("-grt",NFILE,fnm)) {
          snew(grtf,PATH_MAX);
          sprintf(grtf,"%s_%010.2f%s",grtf_base,gnse->dt[i],grtf_ext);
          sprintf(hdr,"G(r,dt), dt = %10.2f",gnse->dt[i]);
          fp = xvgropen(grtf,hdr,"r, nm","G(r,dt)",oenv);
          for(j=0;j<gnse->gr[i]->grn;j++) {
              fprintf(fp,"%10.6lf%10.6lf\n",gnse->gr[i]->r[j],gnse->gr[i]->gr[j]);
          }
          fclose(fp);
          sfree(grtf);
      }
  }
  fprintf(stderr,"\n");
  snew(gnse->sqt,gnse->sq[0]->qn);

  /* now we will gather s(q(t)) from s(q) spectrums */
  for(i=0;i<gnse->sq[0]->qn;i++) {
      snew(gnse->sqt[i],1);
      gnse->sqt[i]->q = gnse->sq[0]->q[i];
      snew(gnse->sqt[i]->s,gnse->nrframes);
      for(j=0;j<gnse->nrframes;j++) {
          gnse->sqt[i]->s[j] = gnse->sq[j]->s[i];
      }
  }

  /* actualy print data */
  for(i=0;i<gnse->sq[0]->qn;i++) {
      snew(sqtf,PATH_MAX);
      sprintf(sqtf,"%s_q_%2.2lf%s",sqtf_base,gnse->sqt[i]->q,sqtf_ext);
      sprintf(hdr,"S(q,dt) , q = %2.2lf",gnse->sqt[i]->q);
      fp = xvgropen(sqtf,hdr,"dt","S(q,dt)",oenv);
      for(j=0;j<gnse->nrframes;j++) {
          fprintf(fp,"%10.6lf%10.6lf\n",gnse->dt[j],gnse->sqt[i]->s[j]);
      }
      fclose(fp);
      sfree(sqtf);
  }

  please_cite(stdout,"Garmay2012");

  thanx(stderr);

  return 0;
}