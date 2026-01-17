/** @file ccbh.c Documented CCBH and SFRD things 
 *
 * Kevin Croker, 19.16.2024
 *
 * No need to class_call() these functions, because they just compute numbers.
 *
 **/

#include "common.h"
#include "background.h"
#include "ccbh.h"

// These function pointers are set dynamically in input.c
double (*ccbh_sfrd)(struct background *, double, double, double) = NULL;
double (*ccbh_Xi)(struct background *, double, double, double) = NULL;

double ccbh_constant_Xi(struct background * pba, double a, double H, double loga) {

  // Set this to noop for now
  return pba->Xi_CCBH;
}

//
// Use this one to amplify early BH formation and suppress
// late formation
//
double ccbh_tanh_Xi(struct background * pba, double a, double H, double loga) {

  double z;

  z = 1./a - 1.;

  //
  // Overall scale it by 2 for now
  // tanh(thing) goes from [-1,1]
  // --> tanh(thing) + 2 from [1, 3], so we get 3x
  // consumption at z > z_trans_Xi_CCBH
  //
  return pba->Xi_CCBH * (2*tanh(10*(z/pba->ztrans_Xi_CCBH - 1.)) + 3.);
}

double ccbh_madau_psi(double z) {

  // KC 6/19/24
  // Madau's \psi is given in Msol/Mpc^3/yr
  // So we need to convert to (CLASS) density and time.
  // NOTE: CLASS density has 8\pi/3 in it too, in addition to
  //       the necessary constants for conversion to geometrized
  //       units.
  return 0.01 * pow(1+z, 2.6)/(1 + pow((1+z)/3.2, 6.2));
}

// This is just a wrapper around the Madau SFRD
// that shifts the units correctly
double ccbh_madau_sfrd(struct background * pba, double a, double H, double loga) {

  return _Mpc2_over_MsolMpc3_ * _Mpc_over_yr_ * ccbh_madau_psi(1/a - 1);
}

//
// Expects the highest provided redshift point to be an explicit zero
//
double ccbh_external_sfrd(struct background * pba, double a, double H, double loga) {
  
  int last_index;
  double result;
  
  // This will get called in the early universe many times, and we don't want to be computing
  // silly exponentials here.
  if(loga < pba->ccbh_external_sfrd_loga[0]) {
    // We're before firstlight, return zero
    result = 0.0;
  }
  else if(loga <= pba->ccbh_external_sfrd_loga[pba->ccbh_external_sfrd_size-1]) {
    class_call(array_interpolate_linear(pba->ccbh_external_sfrd_loga,
					pba->ccbh_external_sfrd_size,
					pba->ccbh_external_sfrd_values,
					1,
					loga,
					&last_index,
					&result,
					1,
					pba->error_message),
	       pba->error_message,
	       pba->error_message);

    // Mollify the value exponentially across its first 5 samples
    if(loga < pba->ccbh_external_sfrd_loga[5])
      result *= exp(1000*(loga - pba->ccbh_external_sfrd_loga[5]));
  }
  else {
    // We're after the last value in the table

    // KC 7/17/24
    // Our behavior is to rescale madau up to where the SFRD cuts off, and
    // then use Madau.  For SFRDs that are larger in normalization than Madau
    // this is justified by Beacom 2006 and the use of faint radio sources,
    // which are not dust-obscured at all, that show maybe 2x larger than
    // Madau SFRDs.  The jump is rarely larger than 2-3x in practice.
    
    // An optimizing compiler should realize that this value is not changing...
    result = pba->ccbh_external_sfrd_values[pba->ccbh_external_sfrd_size-1] / ccbh_madau_psi(1./exp(pba->ccbh_external_sfrd_loga[pba->ccbh_external_sfrd_size-1]) - 1.) * ccbh_madau_psi(1./a - 1.);
  }
  
  //fprintf(stderr, "loga: %.15e, dA_dloga: %.15e\n", loga, result);

  // Scale the result by the correct unit conversion
  return result * _Mpc2_over_MsolMpc3_ * _Mpc_over_yr_;
}

/**
 * This routine reads the SFRD in Msol/Mpc^3 comoving from an external command,
 * and stores the tabulated values.
 * The sampling of the loga's given by the external command is preserved.
 *
 * (Adapted from primordial_external_spectrum_init() by Jesus Torrado)
 *
 * Author: Kevin Croker (kcroker@phys.hawaii.edu)
 * Date:   2024-07-14
 *
 * @param pba  Input/output: pointer to background structure
 * @return the error status
 */

int ccbh_external_sfrd_init(struct background * pba, ErrorMsg errmsg) {
  /** Summary: */

  FILE *process;
  int n_data_guess, n_data = 0;
  double *loga = NULL, *sfrd_data = NULL, *tmp = NULL;
  double this_loga, this_sfrd;
  int status;
  
  /** - Initialization */
  /* Prepare the data (with some initial size) */
  n_data_guess = 30;
  loga    = (double *)tracked_malloc(n_data_guess*sizeof(double));
  sfrd_data = (double *)tracked_malloc(n_data_guess*sizeof(double));

  if(pba->background_verbose > 1)
    printf("CCBH: Fetching external SFRD from\n\t%s\n", pba->ccbh_external_sfrd_file);

  /** - Launch the command and retrieve the output */
  /* Launch the process */
  process = fopen(pba->ccbh_external_sfrd_file, "rt"); 
  class_test(process == NULL,
             errmsg,
             "CCBH: Could not load the sfrd file %s, (errno isn't reliably set, so whatever).",
	     pba->ccbh_external_sfrd_file);

  while(fscanf(process, "%lf %lf\n", &this_loga, &this_sfrd) != EOF) {
    /* Standard technique in C: if too many data, double the size of the vectors */
    /* (it is faster and safer that reallocating every new line) */
    if ((n_data+1) > n_data_guess) {
      n_data_guess *= 2;
      tmp = (double *)tracked_realloc(loga,   n_data_guess*sizeof(double));
      class_test(tmp == NULL,
                 errmsg,
                 "CCBH: Error allocating memory to read external SFRD\n");
      loga = tmp;
      tmp = (double *)tracked_realloc(sfrd_data, n_data_guess*sizeof(double));
      class_test(tmp == NULL,
                 errmsg,
                 "CCBH: Error allocating memory to read external SFRD.\n");
      sfrd_data = tmp;
    }
    
    /* Store */
    loga[n_data]   = this_loga;

    sfrd_data[n_data]   = this_sfrd;

    ++n_data;

    // KC 5/28/23
    // Maybe we can relax this because we will use linear interpolation
    // to make sure we never change the sign of the derivative (unless
    // his splines are fancy and guarantee things like that)
    
    /* Check ascending order of the loga's */
    if (n_data>1) {
      class_test(loga[n_data-1] <= loga[n_data-2],
                 errmsg,
                 "CCBH: SFRD loga's are not strictly sorted in ascending order, "
                 "as it is required for the calculation of the linear interpolation?\n");
    }
  }
  /* Close the file */
  status = fclose(process);
  class_test(status != 0.,
             errmsg,
             "CCBH: Couldn't close the file for external sfrd.  The world is officially ending.");

  /** - Store the read results into CLASS structures */

  pba->ccbh_external_sfrd_size = n_data;

  /** - Make room */

  // KC 7/19/24
  // These were never set to null...
  pba->ccbh_external_sfrd_loga = NULL;
  pba->ccbh_external_sfrd_values = NULL;
  
  class_realloc(pba->ccbh_external_sfrd_loga,
                pba->ccbh_external_sfrd_loga,
                pba->ccbh_external_sfrd_size*sizeof(double),
                errmsg);

  class_realloc(pba->ccbh_external_sfrd_values,
                pba->ccbh_external_sfrd_values,
                pba->ccbh_external_sfrd_size*sizeof(double),
                errmsg);

  /** - Store values */
  // KC 5/28/23
  // These are sort of superfluous now.  Could just set the values directly into the
  // target spot
  memcpy(pba->ccbh_external_sfrd_loga, loga, pba->ccbh_external_sfrd_size*sizeof(double));
  memcpy(pba->ccbh_external_sfrd_values, sfrd_data, pba->ccbh_external_sfrd_size*sizeof(double));

  /** - Release the memory used locally */
  tracked_free(loga);
  tracked_free(sfrd_data);

  /* // KC 5/28/23 */
  /* // DEBUG stuff: output the input table */
  /* for(n_data = 0; n_data < pba->depletion_loga_size; ++n_data) */
  /*   fprintf(stderr, "%e %e\n", pba->depletion_loga[n_data], pba->dA_dloga[n_data]); */
  
  return _SUCCESS_;
}
