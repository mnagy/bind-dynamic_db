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


#include <config.h>

#include <isc/mem.h>
#include <isc/mutex.h>
#include <isc/once.h>
#include <isc/result.h>
#include <isc/types.h>
#include <isc/util.h>

#include <dns/dynamic_db.h>
#include <dns/log.h>
#include <dns/types.h>


#if HAVE_DLFCN_H
#include <dlfcn.h>
#endif

#define CHECK(op)						\
	do { result = (op);					\
		if (result != ISC_R_SUCCESS) goto cleanup;	\
	} while (0)


typedef isc_result_t (*register_func_t)(isc_mem_t *mctx, const char *name,
		const char * const *argv, dns_view_t *view);
typedef void (*destroy_func_t)(void);

typedef struct dyndb_implementation dyndb_implementation_t;

struct dyndb_implementation {
	isc_mem_t			*mctx;
	void				*handle;
	register_func_t			register_function;
	destroy_func_t			destroy_function;
	LINK(dyndb_implementation_t)	link;
};

/* List of implementations. Locked by dyndb_lock. */
static LIST(dyndb_implementation_t) dyndb_implementations;
/* Locks dyndb_implementations. */
static isc_mutex_t dyndb_lock;
static isc_once_t once = ISC_ONCE_INIT;

static void
dyndb_initialize(void) {
	RUNTIME_CHECK(isc_mutex_init(&dyndb_lock) == ISC_R_SUCCESS);
	INIT_LIST(dyndb_implementations);
}


#if HAVE_DLFCN_H
static isc_result_t
load_symbol(void *handle, const char *symbol_name, void **symbol)
{
	const char *errmsg;

	REQUIRE(handle != NULL);
	REQUIRE(symbol != NULL && *symbol == NULL);

	*symbol = dlsym(handle, symbol_name);
	if (*symbol == NULL) {
		errmsg = dlerror();
		if (errmsg == NULL)
			errmsg = "returned function pointer is NULL";
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_DYNDB, ISC_LOG_ERROR,
			      "failed to lookup symbol %s: %s",
			      symbol_name, errmsg);
		return ISC_R_FAILURE;
	}
	dlerror();

	return ISC_R_SUCCESS;
}

static isc_result_t
load_library(isc_mem_t *mctx, const char *filename, dyndb_implementation_t **imp)
{
	isc_result_t result;
	void *handle;
	register_func_t register_function = NULL;
	destroy_func_t destroy_function = NULL;

	REQUIRE(imp != NULL && *imp == NULL);

	handle = dlopen(filename, RTLD_LAZY);
	if (handle == NULL) {
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_DYNDB, ISC_LOG_ERROR,
			      "failed to dynamically load driver '%s': %s",
			      filename, dlerror());
		CHECK(ISC_R_FAILURE);
	}
	dlerror();

	CHECK(load_symbol(handle, "dynamic_driver_init", (void **)&register_function));
	CHECK(load_symbol(handle, "dynamic_driver_destroy", (void **)&destroy_function));

	*imp = isc_mem_get(mctx, sizeof(dyndb_implementation_t));
	if (*imp == NULL)
		CHECK(ISC_R_NOMEMORY);

	(*imp)->mctx = NULL;
	isc_mem_attach(mctx, &((*imp)->mctx));
	(*imp)->handle = handle;
	(*imp)->register_function = register_function;
	(*imp)->destroy_function = destroy_function;
	INIT_LINK(*imp, link);

	return ISC_R_SUCCESS;

cleanup:
	if (handle != NULL)
		dlclose(handle);

	return result;
}

static void
unload_library(dyndb_implementation_t **imp)
{
	REQUIRE(imp != NULL && *imp != NULL);

	dlclose((*imp)->handle);

	isc_mem_putanddetach(&((*imp)->mctx), *imp, sizeof(dyndb_implementation_t));

	*imp = NULL;
}

#else	/* HAVE_DLFCN_H */
static isc_result_t
load_library(isc_mem_t *mctx, const char *filename, dyndb_implementation_t **imp)
{
	UNUSED(mctx);
	UNUSED(filename);
	UNUSED(imp);

	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE, DNS_LOGMODULE_DYNDB,
		      ISC_LOG_ERROR,
		      "dynamic database support is not implemented")

	return ISC_R_NOTIMPLEMENTED;
}

static void
unload_library(dyndb_implementation_t **imp)
{
	REQUIRE(imp != NULL && *imp != NULL);

	isc_mem_putanddetach(&((*imp)->mctx), *imp, sizeof(dyndb_implementation_t));

	*imp = NULL;
}
#endif	/* HAVE_DLFCN_H */

isc_result_t
dns_dynamic_db_load(const char *libname, const char *name, isc_mem_t *mctx,
		    const char * const *argv, dns_view_t *view)
{
	isc_result_t result;
	dyndb_implementation_t *implementation = NULL;

	RUNTIME_CHECK(isc_once_do(&once, dyndb_initialize) == ISC_R_SUCCESS);

	CHECK(load_library(mctx, libname, &implementation));
	CHECK(implementation->register_function(mctx, name, argv, view));

	LOCK(&dyndb_lock);
	APPEND(dyndb_implementations, implementation, link);
	UNLOCK(&dyndb_lock);

	return ISC_R_SUCCESS;

cleanup:
	if (implementation != NULL)
		unload_library(&implementation);

	return result;
}

isc_result_t
dns_dynamic_db_cleanup(void)
{
	dyndb_implementation_t *elem;
	dyndb_implementation_t *next;

	RUNTIME_CHECK(isc_once_do(&once, dyndb_initialize) == ISC_R_SUCCESS);

	LOCK(&dyndb_lock);
	elem = HEAD(dyndb_implementations);
	while (elem != NULL) {
		next = NEXT(elem, link);
		UNLINK(dyndb_implementations, elem, link);
		elem->destroy_function();
		unload_library(&elem);
		elem = next;
	}
	UNLOCK(&dyndb_lock);

	isc_mutex_destroy(&dyndb_lock);
}
