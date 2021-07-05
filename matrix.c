void
multiply_matrices(float a[9], float b[9], float out[9])
{
	// zero out target matrix
	for (int i = 0; i < 9; i++) {
		out[i] = 0;
	}

	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			for (int k = 0; k < 3; k++) {
				out[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
			}
		}
	}
}

void
invert_matrix(const float in[9], float out[9])
{
	float det = in[0] * (in[4] * in[8] - in[5] * in[7]) - in[1] * (in[3] * in[8] - in[5] * in[6]) + in[2] * (in[3] * in[7] - in[4] * in[7]);
	out[0] = (in[4] * in[8] - in[7] * in[5]) / det;
	out[1] = (in[7] * in[2] - in[1] * in[8]) / det;
	out[2] = (in[1] * in[5] - in[4] * in[2]) / det;
	out[3] = (in[6] * in[5] - in[3] * in[8]) / det;
	out[4] = (in[0] * in[8] - in[6] * in[5]) / det;
	out[5] = (in[3] * in[2] - in[0] * in[5]) / det;
	out[6] = (in[3] * in[7] - in[6] * in[4]) / det;
	out[7] = (in[6] * in[1] - in[0] * in[7]) / det;
	out[8] = (in[0] * in[4] - in[3] * in[1]) / det;
}

void
matrix_white_point(const float m[9], float wp[3])
{
	float inv[9];
	invert_matrix(m, inv);
	wp[0] = inv[0] + inv[1] + inv[2];
	wp[1] = inv[3] + inv[4] + inv[5];
	wp[2] = inv[6] + inv[7] + inv[8];
	float max = 0;
	for (int i=0; i<3; i++)
		max = (wp[i] > max) ? wp[i] : max;
	for (int i=0; i<3; i++)
		wp[i] /= max;
}
