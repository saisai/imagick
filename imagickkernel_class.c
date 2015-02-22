/*
   +----------------------------------------------------------------------+
   | PHP Version 5 / Imagick	                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Dan Ackroyd <danack@php.net>                                 |
   +----------------------------------------------------------------------+
*/

#include "php_imagick.h"
#include "php_imagick_defs.h"
#include "php_imagick_macros.h"
#include "php_imagick_helpers.h"

#ifdef IMAGICK_WITH_KERNEL

static void php_imagickkernelvalues_to_zval(zval *zv, KernelInfo *kernel_info) {
	int count;
	double value;
	int x, y;
#ifdef ZEND_ENGINE_3
	zval row;
#else
	zval *row;
#endif

	zval *p_row;

	count = 0;

	for (y=0; y<kernel_info->height ; y++) {
#ifdef ZEND_ENGINE_3
		p_row = &row;
#else
		MAKE_STD_ZVAL(row);
		p_row = row;
#endif

		array_init(p_row);
		for (x=0; x<kernel_info->width ; x++) {
			value = kernel_info->values[count];
			count++;

			//nan is not equal to itself
			if (value != value) {
				//this will be broken by some compilers - need to investigate more...
				add_next_index_bool(p_row, 0);
			}
			else {
				add_next_index_double(p_row, value);
			}
		}

		add_next_index_zval(zv, p_row);
	}
}


HashTable* php_imagickkernel_get_debug_info(zval *obj, int *is_temp TSRMLS_DC) /* {{{ */
{
	php_imagickkernel_object *internp;
	HashTable *retval;
	KernelInfo *kernel_info;
	zval *tmp, zrv;
	zval *row;
#ifdef ZEND_ENGINE_3
	zval matrix;
#else
	zval *matrix;
#endif

	internp = Z_IMAGICKKERNEL_P(obj);
	kernel_info = internp->kernel_info;

	ALLOC_HASHTABLE(retval);
	ZEND_INIT_SYMTABLE_EX(retval, 1, 0);

	INIT_PZVAL(&zrv);

#ifndef ZEND_ENGINE_3
	Z_ARRVAL(zrv) = retval;
#endif

	while (kernel_info != NULL) {
#ifdef ZEND_ENGINE_3
		array_init(&matrix);
		php_imagickkernelvalues_to_zval(&matrix, kernel_info);
		zend_hash_next_index_insert(retval, &matrix);
#else
		MAKE_STD_ZVAL(matrix);
		array_init(matrix);
		php_imagickkernelvalues_to_zval(matrix, kernel_info);
		add_next_index_zval(&zrv, matrix);
#endif
		kernel_info = kernel_info->next;
	}

	return retval;
}


static void im_CalcKernelMetaData(KernelInfo *kernel) {
	size_t i;

	kernel->minimum = kernel->maximum = 0.0;
	kernel->negative_range = kernel->positive_range = 0.0;

	for (i=0; i < (kernel->width*kernel->height); i++) {
		if (fabs(kernel->values[i]) < MagickEpsilon) {
			kernel->values[i] = 0.0;
		}
		if (kernel->values[i] < 0) {
			kernel->negative_range += kernel->values[i];
		}
		else {
			kernel->positive_range += kernel->values[i];
		}
		if (kernel->values[i] < kernel->minimum) {
			kernel->minimum = kernel->values[i];
		}
		
		if (kernel->values[i] > kernel->maximum) {
			kernel->maximum = kernel->values[i];
		}
	}

	return;
}


#if MagickLibVersion > 0x661
static KernelInfo *imagick_createKernel(double *values, size_t width, size_t height, size_t origin_x, size_t origin_y)
{
	KernelInfo *kernel_info;

	kernel_info=AcquireKernelInfo(NULL);
	if (kernel_info == (KernelInfo *) NULL) {
		return NULL;
	}

	kernel_info->width = width;
	kernel_info->height = height;

	kernel_info->x = origin_x;
	kernel_info->y = origin_y;

	//Need to free old values?
	if (kernel_info->values != NULL) {
		RelinquishAlignedMemory(kernel_info->values);
	}

	kernel_info->values = values;
	im_CalcKernelMetaData(kernel_info);

	return kernel_info;
}
#endif

static void createKernelZval(zval *pzval, KernelInfo *kernel_info TSRMLS_DC) {

	php_imagickkernel_object *intern_return;
	php_imagickkernel_object *internp;

	object_init_ex(pzval, php_imagickkernel_sc_entry);
	intern_return = Z_IMAGICKKERNEL_P(pzval);
	intern_return->kernel_info = kernel_info;
}

#define MATRIX_ERROR_EMPTY "Cannot create kernel, matrix is empty."
#define MATRIX_ERROR_UNEVEN "Values must be matrix, with the same number of columns in each row."
#define MATRIX_ERROR_BAD_VALUE "Only numbers or false are valid values in a kernel matrix."
#define MATRIX_ORIGIN_REQUIRED "For kernels with even numbered rows or columns, the origin position must be specified."

/* {{{ proto ImagickKernel ImagickKernel::fromMatrix(array matrix, [array origin])
	Returns a new Kernel from a 2d array of values. The matrix should contain: 
	i) float values where the kernel element should be used 
	ii) false where the element should be skipped
	Each row should have the same number of columns.
	For matrixes with odd number of rows and columns the second parameter can 
	be skipped - the origin will default to the centre of the matrix. For other
	matrixes or if desired the origin co-ordinate can be specified.
*/
#ifdef ZEND_ENGINE_3
PHP_METHOD(imagickkernel, frommatrix)
{
	php_imagickkernel_object *internp;
	php_imagickkernel_object *intern_return;
	zval *kernel_array;
	zval *origin_array;
	HashTable *inner_array;
	KernelInfo *kernel_info;
	long num_rows, num_columns;
	int previous_num_columns;
	int row, column;

	zval *pzval_outer;
	zval *pzval_inner;

	int count = 0;
	size_t origin_x, origin_y;
	zval *tmp;

	double *values = NULL;
	double notanumber = sqrt((double)-1.0);  /* Special Value : Not A Number */

	previous_num_columns = -1;
	count = 0;
	row = 0;
	origin_array = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a|a", &kernel_array, &origin_array) == FAILURE) {
		return;
	}

	num_rows = zend_hash_num_elements(Z_ARRVAL_P(kernel_array));

	if (num_rows == 0) {
		//error - array has zero elements.
		php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_EMPTY TSRMLS_CC);
		return;
	}


	for (row=0 ; row<num_rows ; row++) {
		pzval_outer = zend_hash_index_find(Z_ARRVAL_P(kernel_array), row);
		if (pzval_outer == NULL) {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_UNEVEN TSRMLS_CC);
			goto cleanup;
		}

		zval tmp_zval, *tmp_pzval;
		column = 0;

		if (Z_TYPE_P(pzval_outer) == IS_ARRAY ) {
			inner_array = Z_ARRVAL_P(pzval_outer);
			num_columns = zend_hash_num_elements(inner_array);

			if (num_columns == 0) {
				php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_EMPTY TSRMLS_CC);
				goto cleanup;
			}

			if (values == NULL) {
				values = (double *)AcquireAlignedMemory(num_columns, num_rows*sizeof(double));
			}

			if (previous_num_columns != -1) {
				if (previous_num_columns != num_columns) {
					php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_UNEVEN TSRMLS_CC);
					goto cleanup;
				}
			}

			previous_num_columns = num_columns;

			for (column=0; column<num_columns ; column++) { 
				pzval_inner = zend_hash_index_find(inner_array, row);
				if (pzval_inner == NULL) {
					php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_UNEVEN TSRMLS_CC);
					goto cleanup;
				}

				if (Z_TYPE_P(pzval_inner) == IS_DOUBLE) {
					//It's a float lets use it.
					values[count] = Z_DVAL_P(pzval_inner);
				}
				else if (Z_TYPE_P(pzval_inner) == IS_LONG) {
					//It's a long lets use it.
					values[count] = (float)Z_LVAL_P(pzval_inner);
				}
				else if (Z_TYPE_P(pzval_inner) == IS_FALSE) { 
					//It's false, use nan
					values[count] = notanumber;
				}
				else {
					php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_BAD_VALUE TSRMLS_CC);
					goto cleanup;
				}
				count++;
			}
		}
		else {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_UNEVEN TSRMLS_CC);
			goto cleanup;
		}
	}

	if (origin_array == NULL) {
		if (((num_columns%2) == 0) || ((num_rows%2) == 0)) {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ORIGIN_REQUIRED TSRMLS_CC);
			goto cleanup;
		}
		origin_x = (num_columns - 1) >> 1;
		origin_y = (num_rows - 1) >> 1;
	}
	else {
		HashTable *origin_array_ht;
		origin_array_ht = Z_ARRVAL_P(origin_array);
		tmp = zend_hash_index_find(origin_array_ht, 0);
		if (tmp != NULL) {
			origin_x = Z_LVAL_P(tmp);
		}
		else {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ORIGIN_REQUIRED TSRMLS_CC);
			goto cleanup;
		}
		tmp = zend_hash_index_find(origin_array_ht, 1);
		if (tmp != NULL) {
			origin_y = Z_LVAL_P(tmp);
		}
		else {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ORIGIN_REQUIRED TSRMLS_CC);
			goto cleanup;
		}
	}

	kernel_info = imagick_createKernel(values, num_columns, num_rows, origin_x, origin_y);
	createKernelZval(return_value, kernel_info TSRMLS_CC);

	return;

cleanup:
	if (values != NULL) {
		RelinquishAlignedMemory(values);
	}
}

#else //not ZEND_ENGINE_3


PHP_METHOD(imagickkernel, frommatrix)
{
	php_imagickkernel_object *internp;
	php_imagickkernel_object *intern_return;
	zval *kernel_array;
	zval *origin_array;
	HashTable *inner_array;
	KernelInfo *kernel_info;
	long num_rows, num_columns;
	int previous_num_columns;
	int row, column;

	HashTable *origin_array_ht;
	zval **ppzval_outer;
	zval **ppzval_inner;

	int count = 0;
	size_t origin_x, origin_y;
	zval **tmp;

	double *values = NULL;
	double notanumber = sqrt((double)-1.0);  /* Special Value : Not A Number */

	previous_num_columns = -1;
	count = 0;
	row = 0;
	origin_array = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a|a", &kernel_array, &origin_array) == FAILURE) {
		return;
	}

	num_rows = zend_hash_num_elements(Z_ARRVAL_P(kernel_array));

	if (num_rows == 0) {
		//error - array has zero elements.
		php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_EMPTY TSRMLS_CC);
		return;
	}

	for (row=0 ; row<num_rows ; row++) {
		if (zend_hash_index_find(Z_ARRVAL_P(kernel_array), row, (void **) &ppzval_outer) != SUCCESS) {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_UNEVEN TSRMLS_CC);
			goto cleanup;
		}

		zval tmp_zval, *tmp_pzval;
		column = 0;

		if (Z_TYPE_PP(ppzval_outer) == IS_ARRAY ) {
			//parse this row
			inner_array = Z_ARRVAL_PP(ppzval_outer);
			num_columns = zend_hash_num_elements(inner_array);

			if (num_columns == 0) {
				php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_EMPTY TSRMLS_CC);
				goto cleanup;
			}

			if (values == NULL) {
				values = (double *)AcquireAlignedMemory(num_columns, num_rows*sizeof(double));
			}

			if (previous_num_columns != -1) {
				if (previous_num_columns != num_columns) {
					php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_UNEVEN TSRMLS_CC);
					goto cleanup;
				}
			}

			previous_num_columns = num_columns;

			for (column=0; column<num_columns ; column++) { 
				if (zend_hash_index_find(inner_array, column, (void **) &ppzval_inner) != SUCCESS) {
					php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_UNEVEN TSRMLS_CC);
					goto cleanup;
				}

				if (Z_TYPE_PP(ppzval_inner) == IS_DOUBLE) {
					//It's a float lets use it.
					values[count] = Z_DVAL_PP(ppzval_inner);
				}
				else if (Z_TYPE_PP(ppzval_inner) == IS_LONG) {
					//It's a long lets use it.
					values[count] = (float)Z_LVAL_PP(ppzval_inner);
				}
				else if (Z_TYPE_PP(ppzval_inner) == IS_BOOL && Z_BVAL_PP(ppzval_inner) == 0) { 
					//It's false, use nan
					values[count] = notanumber;
				}
				else {
					php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_BAD_VALUE TSRMLS_CC);
					goto cleanup;
				}
				count++;
			}
		}
		else {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ERROR_UNEVEN TSRMLS_CC);
			goto cleanup;
		}
	}

	if (origin_array == NULL) {
		if (((num_columns%2) == 0) || ((num_rows%2) == 0)) {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ORIGIN_REQUIRED TSRMLS_CC);
			goto cleanup;
		}
		origin_x = (num_columns - 1) >> 1;
		origin_y = (num_rows - 1) >> 1;
	}
	else {
		origin_array_ht = Z_ARRVAL_P(origin_array);
		if (zend_hash_index_find(origin_array_ht, 0, (void**)&tmp) == SUCCESS) {
			origin_x = Z_LVAL_PP(tmp);
		}
		else {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ORIGIN_REQUIRED TSRMLS_CC);
			goto cleanup;
		}

		if (zend_hash_index_find(origin_array_ht, 1, (void**)&tmp) == SUCCESS) {
			origin_y = Z_LVAL_PP(tmp);
		}
		else {
			php_imagick_throw_exception(IMAGICKKERNEL_CLASS, MATRIX_ORIGIN_REQUIRED TSRMLS_CC);
			goto cleanup;
		}
	}

	kernel_info = imagick_createKernel(values, num_columns, num_rows, origin_x, origin_y);
	createKernelZval(return_value, kernel_info TSRMLS_CC);

	return;

cleanup:
	if (values != NULL) {
		RelinquishAlignedMemory(values);
	}
}
#endif //end of zend_engine_3 
/* }}} */

static void imagick_fiddle_with_geometry_info(ssize_t type, GeometryFlags flags, GeometryInfo *geometry_info) {

	/* special handling of missing values in input string */
	switch( type ) {
		/* Shape Kernel Defaults */
		case UnityKernel: {
			if ((flags & WidthValue) == 0)
			geometry_info->rho = 1.0;    /* Default scale = 1.0, zero is valid */
			break;
		}
		case SquareKernel:
		case DiamondKernel:
		case OctagonKernel:
		case DiskKernel:
		case PlusKernel:
		case CrossKernel: {
			if ( (flags & HeightValue) == 0 ) {
				geometry_info->sigma = 1.0;    /* Default scale = 1.0, zero is valid */
			}
			break;
		}
		case RingKernel: {
			if ((flags & XValue) == 0) {
				geometry_info->xi = 1.0;       /* Default scale = 1.0, zero is valid */
			}
			break;
		}
		case RectangleKernel: {    /* Rectangle - set size defaults */
			if ((flags & WidthValue) == 0) { /* if no width then */
				geometry_info->rho = geometry_info->sigma;         /* then  width = height */
			}
			if (geometry_info->rho < 1.0) {            /* if width too small */
				geometry_info->rho = 3;                 /* then  width = 3 */
			}
			if (geometry_info->sigma < 1.0) {          /* if height too small */
				geometry_info->sigma = geometry_info->rho;         /* then  height = width */
			}
			if ((flags & XValue) == 0) {    /* center offset if not defined */
				geometry_info->xi = (double)(((ssize_t)geometry_info->rho-1)/2);
			}
			if ((flags & YValue) == 0) {
				geometry_info->psi = (double)(((ssize_t)geometry_info->sigma-1)/2);
			}
			break;
		}
		/* Distance Kernel Defaults */
		case ChebyshevKernel:
		case ManhattanKernel:
		case OctagonalKernel:
		case EuclideanKernel: {
			if ((flags & HeightValue) == 0) {           /* no distance scale */
				geometry_info->sigma = 100.0;                       /* default distance scaling */
			}
			else if ((flags & AspectValue ) != 0) {     /* '!' flag */
				geometry_info->sigma = QuantumRange/(geometry_info->sigma+1); /* maximum pixel distance */
			}
			else if ((flags & PercentValue ) != 0) {    /* '%' flag */
				geometry_info->sigma *= QuantumRange/100.0;         /* percentage of color range */
			}
			break;
		}
		default: {
			break;
		}
	}
}

/* {{{ proto ImagickKernel ImagickKernel::fromBuiltin(type, string)
*/
PHP_METHOD(imagickkernel, frombuiltin)
{
	php_imagickkernel_object *intern_return;
	long kernel_type;
	GeometryInfo geometry_info = {0};
	KernelInfo *kernel_info;
	char *string;
	IM_LEN_TYPE string_len;
	GeometryFlags flags;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ls", &kernel_type, &string, &string_len) == FAILURE) {
		return;
	}

	flags = ParseGeometry(string, &geometry_info);
	imagick_fiddle_with_geometry_info(kernel_type, flags, &geometry_info);
	kernel_info = AcquireKernelBuiltIn(kernel_type, &geometry_info);
	createKernelZval(return_value, kernel_info TSRMLS_CC);

	return;
}

/* }}} */


/* {{{ proto void ImagickKernel::addKernel(ImagickKernel kernel)
    Attach a kernel to another kernel. Returns the new combined kernel 
*/
PHP_METHOD(imagickkernel, addkernel)
{
	zval *objvar;
	KernelInfo *kernel_info_this_clone;
	KernelInfo *kernel_info_add_clone;

	KernelInfo *kernel_info;
	KernelInfo *kernel_info_target;

	php_imagickkernel_object *kernel;
	php_imagickkernel_object *internp;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O", &objvar, php_imagickkernel_sc_entry) == FAILURE) {
		return;
	}

	kernel = Z_IMAGICKKERNEL_P(objvar);
	internp = Z_IMAGICKKERNEL_P(getThis());
	kernel_info = internp->kernel_info;

	createKernelZval(return_value, kernel_info_this_clone TSRMLS_CC);

	while (kernel_info != NULL) {
		kernel_info_target = kernel_info;
		kernel_info = kernel_info->next;
	};

	kernel_info_add_clone = CloneKernelInfo(kernel->kernel_info);
	kernel_info_target->next = kernel_info_add_clone;

	return;
}
/* }}} */


/* {{{ proto ImagickKernel[] ImagickKernel::separate(void)
	Separate a linked set of kernels into individual kernels
*/
PHP_METHOD(imagickkernel, separate)
{
	php_imagickkernel_object *separate_kernel;
	php_imagickkernel_object *internp;
	KernelInfo *kernel_info;
	KernelInfo *kernel_info_copy;
	int number_values;
	double * values_copy;

#ifdef ZEND_ENGINE_3
	zval separate_object;
#else
	zval *separate_object;
#endif

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	internp = Z_IMAGICKKERNEL_P(getThis());
	kernel_info = internp->kernel_info;

	array_init(return_value);

	while (kernel_info != NULL) {
		number_values = kernel_info->width * kernel_info->height;
		values_copy = (double *)AcquireAlignedMemory(kernel_info->width, kernel_info->height*sizeof(double));
		memcpy(values_copy, kernel_info->values, number_values * sizeof(double));

		kernel_info_copy = imagick_createKernel(
			values_copy,
			kernel_info->width,
			kernel_info->height,
			kernel_info->x,
			kernel_info->y
		);

#ifdef ZEND_ENGINE_3
		createKernelZval(&separate_object, kernel_info_copy TSRMLS_CC);
		add_next_index_zval(return_value, &separate_object);
#else 
		MAKE_STD_ZVAL(separate_object);
		createKernelZval(separate_object, kernel_info_copy TSRMLS_CC);
		add_next_index_zval(return_value, separate_object);
#endif
		kernel_info = kernel_info->next;
	}

	return;
}
/* }}} */


/* {{{ proto [] ImagickKernel::getMatrix(void)
	Return the values used in the kernel. The array contains floats
	for the elements used and false for the elements not used.
*/
PHP_METHOD(imagickkernel, getmatrix)
{
	php_imagickkernel_object *internp;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	internp = Z_IMAGICKKERNEL_P(getThis());

	array_init(return_value);
	php_imagickkernelvalues_to_zval(return_value, internp->kernel_info);

	return;
}
/* }}} */





/* {{{ proto [] ImagickKernel::scale(float scaling_factor[, int NORMALIZE_KERNEL_FLAG])
	 Adds a given amount of the 'Unity' Convolution Kernel to the given pre-scaled and normalized Kernel. This in effect adds that amount of the original image into the resulting convolution kernel. The resulting effect is to convert the defined kernels into blended soft-blurs, unsharp kernels or into sharpening kernels.
*/
PHP_METHOD(imagickkernel, scale)
{
	php_imagickkernel_object *internp;
	double scale;
	long normalize_flag;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "d|l", &scale, &normalize_flag) == FAILURE) {
		return;
	}

	internp = Z_IMAGICKKERNEL_P(getThis());
	ScaleKernelInfo(internp->kernel_info, scale, normalize_flag);

	return;
}
/* }}} */




/* {{{ proto [] ImagickKernel::addUnityKernel(float scale)
	 Adds a given amount of the 'Unity' Convolution Kernel to the given pre-scaled and normalized Kernel. This in effect adds that amount of the original image into the resulting convolution kernel. The resulting effect is to convert the defined kernels into blended soft-blurs, unsharp kernels or into sharpening kernels.
*/
PHP_METHOD(imagickkernel, addunitykernel)
{
	php_imagickkernel_object *internp;
	double scale;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "d", &scale) == FAILURE) {
		return;
	}

	internp = Z_IMAGICKKERNEL_P(getThis());
	UnityAddKernelInfo(internp->kernel_info, scale);

	return;
}
/* }}} */

#endif