/* udb_obj.c - Binary object handling gear. */
/* $Id$ */

/* Originally written by Andrew Molitor, amolitor@nmsu.edu 1992 */    

/* Why not just write the attributes individually to disk? Well, when you're
 * running on a platform that does synchronous writes with a large database,
 * thousands of I/O operations tend to be expensive. When you 'coalesce'
 * many attribute writes onto a single object and do only one I/O operation,
 * you can get an order of magnitude speed difference, especially on
 * loading/unloading to flatfile. It also has the side effect of
 * pre-fetching on reads, since you often have sequential attribute reads
 * off of the same object. */
   
#include "copyright.h"
#include "autoconf.h" 
#include "config.h"   

#include "alloc.h"      /* required by mudconf */
#include "flags.h"      /* required by mudconf */
#include "htab.h"       /* required by mudconf */
#include "mudconf.h"    /* required by code */   

#include "db.h"         /* required by externs */
#include "externs.h"    /* required by code */   

#include "udb.h"        /* required by code */

/* Sizes, on disk, of Object and (within the object) Attribute headers */

#define OBJ_HEADER_SIZE		(sizeof(Objname) + sizeof(int))
#define ATTR_HEADER_SIZE	(sizeof(int) * 2)

/* Take a chunk of data which contains an object, and parse it into an
 * object structure. */

Obj *unroll_obj(data)
char *data;
{
	int i, j;
	Obj *o;
	Attrib *a;
	char *dptr;
	
	dptr = data;
	
	/* Get a new Obj struct */

	if ((o = (Obj *) XMALLOC(sizeof(Obj), "unroll_obj.o")) == NULL)
		return (NULL);

	/* Read in the header */

	if (memcpy((void *)&(o->name), (void *)dptr, sizeof(Objname)) == NULL) {
		XFREE(o, "unroll_obj.o");
		return (NULL);
	}
	dptr += sizeof(Objname);
	
	if (memcpy((void *)&i, (void *)dptr, sizeof(int)) == NULL) {
		XFREE(o, "unroll_obj.o");
		return (NULL);
	}
	dptr += sizeof(int);
	
	o->at_count = i;

	/* Now get an array of Attrs */

	a = o->atrs = (Attrib *) XMALLOC(i * sizeof(Attrib), "unroll_obj.a");
	if (!o->atrs) {
		XFREE(o, "unroll_obj.o");
		return (NULL);
	}
	
	/* Now go get the attrs, one at a time. */

	for (j = 0; j < i;) {

		/* Attribute size */

		if (memcpy((void *)&(a[j].size), (void *)dptr, sizeof(int)) == NULL)
			 goto bail;
		dptr += sizeof(int);

		/* Attribute number */

		if (memcpy((void *)&(a[j].attrnum), (void *)dptr, sizeof(int)) == NULL)
			 goto bail;
		dptr += sizeof(int);
		
		/* get some memory for the data */

		if ((a[j].data = (char *)XMALLOC(a[j].size, "unroll_obj.data")) == NULL)
			goto bail;

		/* Preincrement j, so we know how many to free if this next 
		 * bit fails. */

		j++;

		/* Now get the data */

		if (memcpy((void *)a[j - 1].data, (void *)dptr, a[j - 1].size) == NULL)
			goto bail;
		dptr += a[j - 1].size;
	}


	/* Should be all done... */

	o->dirty = 0;
	return (o);

	/* Oh shit. We gotta free up all these little bits of memory. */
bail:
	/* j points one attribute *beyond* what we need to free up */

	for (i = 0; i < j; i++)
		XFREE(a[i].data, "unroll_obj.data");

	XFREE(a, "unroll_obj.a");
	XFREE(o, "unroll_obj.o");

	return (NULL);
}

/* Rollup an object structure into a single buffer for a write to disk. */

int rollup_obj(o, data)
Obj *o;
char **data;
{
	int i;
	Attrib *a;
	char *dptr;

	if (!data)
		return (-1);

	dptr = *data = (char *)XMALLOC(obj_siz(o), "rollup_obj.data");

	/* Mark the object as clean */
	
	o->dirty = 0;

	/* Write out the object header */

	if (memcpy((void *)dptr, (void *)&(o->name), sizeof(Objname)) == NULL)
		return (-1);
	dptr += sizeof(Objname);

	if (memcpy((void *)dptr, (void *)&(o->at_count), sizeof(int)) == NULL)
		 return (-1);
	dptr += sizeof(int);

	/* Now do the attributes, one at a time. */

	a = o->atrs;
	for (i = 0; i < o->at_count; i++) {

		/* Attribute size. */

		if (memcpy((void *)dptr, (void *)&(a[i].size), sizeof(int)) == NULL)
			 return (-1);
		dptr += sizeof(int);

		/* Attribute number */

		if (memcpy((void *)dptr, (void *)&(a[i].attrnum), sizeof(int)) == NULL)
			 return (-1);
		dptr += sizeof(int);


		/* Attribute data */

		if (memcpy((void *)dptr, (void *)a[i].data, a[i].size) == NULL)
			return (-1);
		dptr += a[i].size;
	}

	/* That's all she wrote. */

	return (0);
}

/* Return the size, on disk, the thing is going to take up.*/

int obj_siz(o)
Obj *o;
{
	int i;
	int siz;

	siz = OBJ_HEADER_SIZE;

	for (i = 0; i < o->at_count; i++)
		siz += (((o->atrs)[i]).size + ATTR_HEADER_SIZE);

	return (siz);
}

/* And something to free all the goo on an Obj, as well as the Obj.*/

static void objfree(o)
Obj *o;
{
	int i;
	Attrib *a;

	if (!o->atrs) {
		XFREE(o, "objfree.o");
		return;
	}
	a = o->atrs;
	for (i = 0; i < o->at_count; i++) {
		XFREE(a[i].data, "objfree.data");
	}

	XFREE(a, "objfree.a");
	XFREE(o, "objfree.o");
}

/* Routines to manipulate attributes within the object structure */

char *get_attrib(anam, obj)
int anam;
Obj *obj;
{
	int lo, mid, hi;
	Attrib *a;

	/* Binary search for the attribute */

	lo = 0;
	hi = obj->at_count - 1;
	a = obj->atrs;
	while (lo <= hi) {
		mid = ((hi - lo) >> 1) + lo;
		if (a[mid].attrnum == anam) {
			return (char *) a[mid].data;
			break;
		} else if (a[mid].attrnum > anam) {
			hi = mid - 1;
		} else {
			lo = mid + 1;
		}
	}
	return (NULL);
}

void set_attrib(anam, obj, value)
int anam;
Obj *obj;
char *value;
{
	int hi, lo, mid;
	Attrib *a;

	/* Demands made elsewhere insist that we cope with the case of an
	 * empty object. */

	if (obj->atrs == NULL) {
		a = (Attrib *) XMALLOC(sizeof(Attrib), "set_attrib.a");
		obj->atrs = a;
		obj->at_count = 1;
		a[0].attrnum = anam;
		a[0].data = (char *)value;
		a[0].size = strlen(value) + 1;
		return;
	}

	/* Binary search for the attribute. */

	lo = 0;
	hi = obj->at_count - 1;

	a = obj->atrs;
	while (lo <= hi) {
		mid = ((hi - lo) >> 1) + lo;
		if (a[mid].attrnum == anam) {
			XFREE(a[mid].data, "set_attrib");
			a[mid].data = (char *)value;
			a[mid].size = strlen(value) + 1;
			return;
		} else if (a[mid].attrnum > anam) {
			hi = mid - 1;
		} else {
			lo = mid + 1;
		}
	}

	/* If we got here, we didn't find it, so lo = hi + 1, and the
	 * attribute should be inserted between them. */

	a = (Attrib *) XREALLOC(obj->atrs, 
		(obj->at_count + 1) * sizeof(Attrib), "set_attrib.a");

	/* Move the stuff upwards one slot. */

	if (lo < obj->at_count)
		memmove((void *)(a + lo + 1), (void *)(a + lo),
		      (obj->at_count - lo) * sizeof(Attrib));

	a[lo].data = value;
	a[lo].attrnum = anam;
	a[lo].size = strlen(value) + 1;
	obj->at_count++;
	obj->atrs = a;
}

void del_attrib(anam, obj)
int anam;
Obj *obj;
{
	int hi, lo, mid;
	Attrib *a;

	if (!obj->at_count || !obj->atrs)
		return;

	if (obj->at_count < 0)
		abort();

	/* Binary search for the attribute. */

	lo = 0;
	hi = obj->at_count - 1;
	a = obj->atrs;
	while (lo <= hi) {
		mid = ((hi - lo) >> 1) + lo;
		if (a[mid].attrnum == anam) {
			XFREE(a[mid].data, "del_attrib.data");
			obj->at_count--;
			if (mid != obj->at_count)
				memcpy((void *)(a + mid), (void *)(a + mid + 1), 
				    (obj->at_count - mid) * sizeof(Attrib));

			if (obj->at_count == 0) {
				XFREE(obj->atrs, "del_attrib.atrs");
				obj->atrs = NULL;
			}
			return;
		} else if (a[mid].attrnum > anam) {
			hi = mid - 1;
		} else {
			lo = mid + 1;
		}
	}
}

/* Now let's define the functions that the cache actually uses. These
 * 'pipeline' attribute writes using a pair of object structures. When
 * the object which is being read or written to changes, we have to
 * dump the current object, and bring in the new one. */

char *fetch_attrib(anum, obj)
int anum;
int obj;
{
	char *data;
	Obj *object;
		
	if (mudstate.read_object && mudstate.read_object->name == obj) {
		object = mudstate.read_object;
	} else if (mudstate.write_object && mudstate.write_object->name == obj) {
		object = mudstate.write_object;
	} else if (!mudstate.read_object) {
		/* If there's no object there, read one in */
		
		ATTRS_FETCH(obj, &data);
		if (data) {
			mudstate.read_object = unroll_obj(data);
			object = mudstate.read_object;
			XFREE(data, "fetch_attrib.1");
		} else {
			/* New object */
			if ((mudstate.read_object = (Obj *) XMALLOC(sizeof(Obj), 
			                         "unroll_obj.o")) == NULL)
				return (NULL);
			object = mudstate.read_object;
			object->name = obj;
			object->at_count = 0;
			object->dirty = 0;
			object->atrs = NULL;
		}
	} else {
		/* If it's dirty, write it first */
	
		if (mudstate.read_object->dirty) {
			rollup_obj(mudstate.read_object, &data);
			ATTRS_STORE(mudstate.read_object->name, data,
				obj_siz(mudstate.read_object));
			XFREE(data, "fetch_attrib.3");
		}
		objfree(mudstate.read_object);
		ATTRS_FETCH(obj, &data);
		if (data) {
			mudstate.read_object = unroll_obj(data);
			object = mudstate.read_object;
			XFREE(data, "fetch_attrib.2");
		} else {
			/* New object */
			if ((mudstate.read_object = (Obj *) XMALLOC(sizeof(Obj), 
	                        "unroll_obj.o")) == NULL)
				return (NULL);
			object = mudstate.read_object;
			object->name = obj;
			object->at_count = 0;
			object->dirty = 0;
			object->atrs = NULL;
		}
	}
	return get_attrib(anum, object);
}

void put_attrib(anum, obj, value)
int anum;
int obj;
char *value;
{
	char *data;
	Obj *object;
	
	if (mudstate.write_object && mudstate.write_object->name == obj) {
		object = mudstate.write_object;
	} else if (mudstate.read_object && mudstate.read_object->name == obj) {
		object = mudstate.read_object;
	} else if (!mudstate.write_object) {
		/* If there's no object there, read one in */
		
		ATTRS_FETCH(obj, &data);
		if (data) {
			mudstate.write_object = unroll_obj(data);
			object = mudstate.write_object;
			XFREE(data, "fetch_attrib.1");
		} else {
			/* New object */
			if ((mudstate.write_object = (Obj *) XMALLOC(sizeof(Obj), 
			                         "unroll_obj.o")) == NULL)
				return;
			object = mudstate.write_object;
			object->name = obj;
			object->at_count = 0;
			object->dirty = 0;
			object->atrs = NULL;
		}
	} else {
		/* If it's dirty, write it first */
	
		if (mudstate.write_object->dirty) {
			rollup_obj(mudstate.write_object, &data);
			ATTRS_STORE(mudstate.write_object->name, data,
				obj_siz(mudstate.write_object));
			XFREE(data, "fetch_attrib.3");
		}
		objfree(mudstate.write_object);
		ATTRS_FETCH(obj, &data);
		if (data) {
			mudstate.write_object = unroll_obj(data);
			object = mudstate.write_object;
			XFREE(data, "fetch_attrib.2");
		} else {
			/* New object */
			if ((mudstate.write_object = (Obj *) XMALLOC(sizeof(Obj), 
	                        "unroll_obj.o")) == NULL)
				return;
			object = mudstate.write_object;
			object->name = obj;
			object->at_count = 0;
			object->dirty = 0;
			object->atrs = NULL;
		}
	}

	/* Write the damn thing */
	
	object->dirty = 1;
	set_attrib(anum, object, value);
	return;
}

void delete_attrib(anum, obj)
int anum;
int obj;
{
	char *data;
	Obj *object;
	
	if (mudstate.write_object && mudstate.write_object->name == obj) {
		object = mudstate.write_object;
	} else if (mudstate.read_object && mudstate.read_object->name == obj) {
		object = mudstate.read_object;
	} else if (!mudstate.write_object) {
		/* If there's no object there, read one in */
		
		ATTRS_FETCH(obj, &data);
		if (data) {
			mudstate.write_object = unroll_obj(data);
			object = mudstate.write_object;
			XFREE(data, "fetch_attrib.1");
		} else {
			/* New object */
			if ((mudstate.write_object = (Obj *) XMALLOC(sizeof(Obj), 
			                         "unroll_obj.o")) == NULL)
				return;
			object = mudstate.write_object;
			object->name = obj;
			object->at_count = 0;
			object->dirty = 0;
			object->atrs = NULL;
		}
	} else {
		/* If it's dirty, write it first */
	
		if (mudstate.write_object->dirty) {
			rollup_obj(mudstate.write_object, &data);
			ATTRS_STORE(mudstate.write_object->name, data,
				obj_siz(mudstate.write_object));
			XFREE(data, "fetch_attrib.3");
		}
		objfree(mudstate.write_object);
		ATTRS_FETCH(obj, &data);
		if (data) {
			mudstate.write_object = unroll_obj(data);
			object = mudstate.write_object;
			XFREE(data, "fetch_attrib.2");
		} else {
			/* New object */
			if ((mudstate.write_object = (Obj *) XMALLOC(sizeof(Obj), 
	                        "unroll_obj.o")) == NULL)
				return;
			object = mudstate.write_object;
			object->name = obj;
			object->at_count = 0;
			object->dirty = 0;
			object->atrs = NULL;
		}
	}
	
	/* Write the damn thing */

	object->dirty = 1;
	del_attrib(anum, object);
	return;
	
}

void attrib_sync()
{
	char *data;

	/* Make sure dirty objects are written to disk */
	
	if (mudstate.write_object && mudstate.write_object->dirty) {
		rollup_obj(mudstate.write_object, &data);
		ATTRS_STORE(mudstate.write_object->name, data,
			obj_siz(mudstate.write_object));
		XFREE(data, "attrib_sync.1");
	}

	if (mudstate.read_object && mudstate.read_object->dirty) {
		rollup_obj(mudstate.read_object, &data);
		ATTRS_STORE(mudstate.read_object->name, data,
			obj_siz(mudstate.read_object));
		XFREE(data, "attrib_sync.2");
	}
}
