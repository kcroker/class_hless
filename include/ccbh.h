/** @file ccbh.h Documented includes for CCBH and star formation rate density things */

#ifndef __CCBH__
#define __CCBH__

#include "common.h"

// Labeled in the notation of thermodynamics.h
// This contains the 8piG/3c^2 required for CLASS units
// See background.c:631 and thereabouts
#define _Mpc2_over_MsolMpc3_ 4.0090205322280357e-19  /**< conversion factor from astronomy units Msol/Mpc^3 to CLASS_rho 1/Mpc^2 (rho in CLASS_rho = (this const)*Msol/Mpc^3) */

// This one is just a direct conversion, using c
#define _Mpc_over_yr_ 3.26156377716743340716e+06 /**< conversion factor from astronomy units 1/years to CLASS_rate 1/Mpc (rate in CLASS_rate = (this const)*1/yr */

// So that the function pointers are available elsewhere
extern double (*ccbh_sfrd)(struct background *, double, double, double);
extern double (*ccbh_Xi)(struct background *, double, double, double);

// Function prototypes
int ccbh_external_sfrd_init(struct background * pba, ErrorMsg errmsg);
  
double ccbh_madau_psi(double z);
double ccbh_madau_sfrd(struct background * pba, double a, double H, double loga);
double ccbh_external_sfrd(struct background * pba, double a, double H, double loga);
double ccbh_constant_Xi(struct background * pba, double a, double H, double loga);
double ccbh_tanh_Xi(struct background * pba, double a, double H, double loga);

// Types of background model
enum ccbh_background_model {instant};

#endif
