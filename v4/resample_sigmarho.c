#include "mex.h"
#include "matrix.h"

/* Fills in missing elements in an inv_sigma (as indicated by 0xFFFFFFFF entries) */
inline void fill_in_inv_sigma(unsigned int* inv_sigma)
{
	unsigned int i, j;
	unsigned int empty_ranks_idx = 0;
	unsigned int* rank_perm;

	memset(g_is_item_missing, 0x01, g_n * sizeof(char));

	/* Identify ranks without items, and the missing items themselves */
	for (i = 0; i < g_n; ++i)
	{
		if (inv_sigma[i] == 0xFFFFFFFF) /* This equality only works with unsigned int! */
		{
			g_empty_ranks[empty_ranks_idx++] = i;
		}
		else
		{
			g_is_item_missing[inv_sigma[i]] = 0;
		}
	}

	if (empty_ranks_idx == 0)
	{
		return;
	}

	/* Perform random permutation of those ranks */
	rank_perm = randperm(empty_ranks_idx);
	j = 0;
	for (i = 0; i < g_n; ++i)
	{
		if (g_is_item_missing[i] == 0x01)
		{
			/* Fill a random empty position with this value */
			inv_sigma[g_empty_ranks[rank_perm[j++]]] = i;
		}
	}
	mxFree(rank_perm);
}

/* Keeps g_S up to date for given cluster; assumes current cluster is in g_cluster_elements */
inline void update_S_vector(unsigned int c)
{
	unsigned int i, j;
	unsigned int* sigma = g_sigma + index2d(0, g_n, c);
	unsigned int* S = g_S + index2d(0, g_max_t, c);

	/* NOTE: Alternative technique is to sum up lower diagonals of R matrices, but I think that's actually slower
	         for smaller clusters; this part shouldn't be the bottleneck anyway */
	memset(S, 0, g_max_t * sizeof(unsigned int));
	for (i = 0; i < g_num_cluster_elements; ++i)
	{
		relative_s(g_current_s, sigma, g_inv_pi + index2d(0, g_max_t, g_cluster_elements[i]), g_t[g_cluster_elements[i]]);
		for (j = 0; j < g_max_t; ++j)
		{
			S[j] += g_current_s[j];
		}
	}
}

/* Computes R = \sum_j \theta_j R_j */
inline void compute_R_matrix(unsigned int c)
{
	/* This is easy, just sum all the R_j matrices up */
	unsigned int j, i;
	unsigned int g_n2 = g_n * g_n;
	unsigned int* R_j;
	double rho;
	memset(g_R, 0, g_n2 * sizeof(double));
	for (j = 0; j < g_max_t; ++j)
	{
		rho = g_rho[index2d(j, g_max_t, c)];
		R_j = g_R_j + index3d(0, g_n, 0, g_n, j);
		for (i = 0; i < g_n2; ++i)
		{
			g_R[i] += rho * (double)R_j[i];
		}
	}
}

/* Computes R_j, the sufficient stats matrices, for j = 1 to max_t */
/* Assumes g_cluster_elements is filled with items of current cluster */
inline void compute_R_j_matrix(unsigned int c)
{
	/* The sufficient stats are precomputed in sparse format, so here we just add them up */
	unsigned int i, j, k;
	unsigned int* pi_R_j;
	unsigned int* R_j;

	memset(g_R_j, 0, g_n * g_n * g_max_t * sizeof(unsigned int));
	for (i = 0; i < g_num_cluster_elements; ++i)
	{
		for (j = 0; j < g_t[g_cluster_elements[i]]; ++j)
		{
			assert(g_c[g_cluster_elements[i]] == c);
			/* Add this cluster's R to suff stat */
			R_j = g_R_j + index3d(0, g_n, 0, g_n, j);
			pi_R_j = g_pi_R + index3d(0, g_n - 1, j, g_max_t, g_cluster_elements[i]);

			/* There should always be exactly g_n - j - 1 entries here */
			for (k = 0; k < g_n - j - 1; ++k)
			{
				/* Increment indicated entry; takes advantage of linear indexing */
				++R_j[pi_R_j[k]];
			}
		}
	}
}

/* Performs exact sampling of sigma assuming that there is only one permutation assigned to the cluster */
inline void resample_inv_sigma_exact(unsigned int c, unsigned int idx)
{
	unsigned int i, j;
	unsigned int t = g_t[idx];
	unsigned int rank;
	unsigned int* inv_sigma = g_inv_sigma + index2d(0, g_n, c);
	unsigned int* inv_pi = g_inv_pi + index2d(0, g_max_t, idx);
	assert(g_nc[index2d(0, g_max_t, c)] == 1 && g_c[idx] == c);

	/* For each rank position: sample an s position of this single permutation from the centroid */
	for (i = 0; i < t; ++i)
	{
		g_current_s[i] = sample_normalized(g_beta_table + index2d(0, g_n, i), g_n - i, g_inverseTemperature, g_unifrand[g_randIdx++]);
	}

	/* All ones setting corresponds to unset values */
	memset(inv_sigma, 0xFF, g_n * sizeof(unsigned int));

	for (i = 0; i < t; ++i)
	{
		j = 0;
		rank = 0;
		/* Increase rank by previously observed items that are ahead of this in the permutation */
		for (j = 0; j < g_n; ++j)
		{
			if (inv_sigma[j] == 0xFFFFFFFF)
			{
				/* Unoccupied space; can increase rank */
				++rank;
				if (rank > g_current_s[i])
				{
					break;
				}
			}
		}
		assert(j < g_n && inv_sigma[j] == 0xFFFFFFFF);
		inv_sigma[j] = inv_pi[i];
	}

	/* Fill in remaining empty entries */
	fill_in_inv_sigma(inv_sigma);

	/* Update sigma and S */
	invert_pi(g_sigma + index2d(0, g_n, c), inv_sigma, g_n);
	update_S_vector(c);
}

/* Performs conditional sampling of dispersion parameters rho given a cluster centroid */
/* This is the case using the Beta approximation */
inline void resample_rho_beta(unsigned int c)
{
	/* NOTE: faster implementation would be to do this call to betarnd all at once for all clusters
	         but that means using a lot more space (keeping all R_j matrices for each cluster around) */
	unsigned int i;
	size_t idx = index2d(0, g_max_t, c);

	mxArray* betarnd_out_array;
	double* betarnd_out;

	/* Prepare inputs to betarnd */
	for (i = 0; i < g_max_t; ++i)
	{
		g_alpha_param[i] = g_nu_0 * g_r_0[i] + (double)g_S[idx + i];
		g_beta_param[i] = g_nu_0 * (double)g_nc[idx + i] + 1.0;
	}

	mexCallMATLAB(1, &betarnd_out_array, 2, g_betarnd_params, "betarnd");
	betarnd_out = mxGetData(betarnd_out_array);

	/* Take -log() of that expression */
	for (i = 0; i < g_max_t; ++i)
	{
		betarnd_out[i] = -log(betarnd_out[i]);
	}

	/* Copy resulting samples into rho */
	memcpy(g_rho + idx, betarnd_out, g_max_t * sizeof(double));

	mxDestroyArray(betarnd_out_array);
}

/* Performs conditional sampling of dispersion parameters rho given a cluster centroid */
/* This is the case using the slice sampler */
inline void resample_rho_slice(unsigned int c)
{
	unsigned int i, j;
	int status;
	mxArray* rho_out_array;

	size_t idx = index2d(0, g_max_t, c);

	/* Prepare inputs to betarnd */
	for (i = 0; i < g_max_t; ++i)
	{
		*g_coeff_param =  g_nu_0 * g_r_0[i] + (double)g_S[idx + i];
		*g_n_minus_j_param = (double)g_n - (double)i;
		*g_nu_param = g_nu_0 + g_nc[idx + i];
		*g_last_rho_param = g_rho[idx + i];

		for (j = 0; j < SLICE_SAMPLE_RETRY_LIMIT; ++j)
		{
			status = mexCallMATLAB(1, &rho_out_array, 5, g_resample_rho_slice_params, "resample_rho_slice");
			if (status == 0)
			{
				break;
			}
			mexPrintf("Warning: slice sampling failed on cluster %d, rho[%d]\n", c, i);
		}
		if (j == 3)
		{
			mexErrMsgTxt("Could not slice resample rho\n");
		}

		g_rho[index2d(i, g_max_t, c)] = mxGetScalar(rho_out_array);

		mxDestroyArray(rho_out_array);
	}
}

/* Performs conditional sampling of sigma given a set of dispersion parameters rho */
inline void resample_inv_sigma(unsigned int c)
{
	/* Performs stagewise sampling of inv_sigma */

	unsigned int i, j;
	unsigned int* inv_sigma = g_inv_sigma + index2d(0, g_n, c);

	compute_R_matrix(c);

	/* All ones setting corresponds to unset values */
	memset(inv_sigma, 0xFF, g_n * sizeof(unsigned int));

	/* Calculate negative column sums */
	memset(g_R_column_sums, 0, g_n * sizeof(double));
	for (i = 0; i < g_n; ++i)
	{
		/* k iterates through rows */
		for (j = 0; j < g_n; ++j)
		{
			g_R_column_sums[i] -= g_R[index2d(j, g_n, i)];
		}
	}

	for (i = 0; i < g_n; ++i)
	{
		/* Sample according to column sums (conveniently we can pretend these are log space) */
		inv_sigma[i] = sample_normalized_log(g_R_column_sums, g_sigma_prob, g_n, g_inverseTemperature, g_unifrand[g_randIdx++]);
		assert(g_R_column_sums[inv_sigma[i]] != -DBL_MAX);

		/* "Zero out" column sum probability */
		g_R_column_sums[inv_sigma[i]] = -DBL_MAX;

		/* Clear out this probability mass from all column sums */
		for (j = 0; j < g_n; ++j)
		{
			if (g_R_column_sums[j] < 0.0)
			{
				g_R_column_sums[j] += (double)g_R[index2d(inv_sigma[i], g_n, j)];
				assert(g_R_column_sums[j] < 1e-10);
				if (g_R_column_sums[j] > 0.0)
				{
					g_R_column_sums[j] = 0.0;
				}
			}
		}
	}

	/* Update sigma and S */
	invert_pi(g_sigma + index2d(0, g_n, c), inv_sigma, g_n);
	update_S_vector(c);
}

/* Performs resampling of cluster parameters, by doing rho | sigma and sigma | rho */
void resample_sigmarho(unsigned int c)
{
	unsigned int i;

	/* Gather cluster elements */
	g_num_cluster_elements = 0;
	for (i = 0; i < g_N; ++i)
	{
		if (g_c[i] == c)
		{
			g_cluster_elements[g_num_cluster_elements++] = i;
		}
	}

	/* Different operations for singleton vs. multi-clusters */
	/* Note that we always use the stagewise sampler if we are using SLICE-GIBBS (since the N = 1 case is an approximation) */
	if (g_model == BETA_GIBBS && g_nc[index2d(0, g_max_t, c)] == 1)
	{
		assert(g_num_cluster_elements == 1);

		/* Single item cluster, can do exact sampling given specific permutation */
		resample_inv_sigma_exact(c, g_cluster_elements[0]);
	}
	else
	{
		compute_R_j_matrix(c);

		/* Iterate back and forth between sigma and rho */
		for (i = 0; i < g_sigma_gibbs; ++i)
		{
			/* Re-estimate rho - important that this is done first! */
			if (g_model == BETA_GIBBS)
			{
				resample_rho_beta(c);
			}
			else
			{
				resample_rho_slice(c);
			}

			/* Re-estimate sigma */
			resample_inv_sigma(c);
		}
	}
}


