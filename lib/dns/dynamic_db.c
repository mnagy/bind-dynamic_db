/*
 * Copyright (C) 2004-2009  Internet Systems Consortium, Inc. ("ISC")
 * Copyright (C) 1996-2003  Internet Software Consortium.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */


#include <isc/result.h>
#include <isc/types.h>
#include <isc/util.h>

#include <dns/dynamic_db.h>
#include <dns/log.h>
#include <dns/types.h>


/* TODO: Adjust configure.ac accordingly. */
#define HAVE_DLOPEN 1

#if HAVE_DLOPEN
#include <dlfcn.h>
#endif

#define CHECK(op)						\
	do { result = (op);					\
		if (result != ISC_R_SUCCESS) goto cleanup;	\
	} while (0)

typedef isc_result_t (*register_func_t)(isc_mem_t *mctx, const char *name,
		const char * const *argv, dns_view_t *view);

#if HAVE_DLOPEN
static isc_result_t
load_library(const char *filename, register_func_t *register_function)
{
	isc_result_t result;
	void *handle;	/* XXX: We don't keep the handle around. Should we? */
	const char *errmsg;

	REQUIRE(register_function != NULL && *register_function == NULL);

	handle = dlopen(filename, RTLD_LAZY);
	if (handle == NULL) {
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_DYNDB, ISC_LOG_ERROR,
			      "Failed to dynamically load driver '%s': %s",
			      filename, dlerror());
		CHECK(ISC_R_FAILURE);
	}
	dlerror();

	*register_function = dlsym(handle, "dynamic_driver_init");
	if (*register_function == NULL) {
		errmsg = dlerror();
		if (errmsg == NULL)
			errmsg = "returned function pointer is NULL";
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_DYNDB, ISC_LOG_ERROR,
			      "Failed to lookup symbol dynamic_driver_init from %s: %s",
			      filename, errmsg);
		CHECK(ISC_R_FAILURE);
	}
	dlerror();

	return ISC_R_SUCCESS;

cleanup:
	if (handle != NULL)
		dlclose(handle);
	*register_function = NULL;

	return result;
}
#else
static isc_result_t
load_library(const char *filename, register_func_t *register_function)
{
	UNUSED(filename);
	UNUSED(register_function);

	return ISC_R_NOTIMPLEMENTED;
}
#endif

isc_result_t
dns_dynamic_db_load(const char *libname, const char *name, isc_mem_t *mctx,
		    const char * const *argv, dns_view_t *view)
{
	isc_result_t result;
	register_func_t register_func = NULL;

	CHECK(load_library(libname, &register_func));
	CHECK(register_func(mctx, name, argv, view));

cleanup:
	return result;
}
