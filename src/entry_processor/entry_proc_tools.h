/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright (C) 2008, 2009 CEA/DAM
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the CeCILL License.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL license (http://www.cecill.info) and that you
 * accept its terms.
 */
/**
 * Misc tools for managing entry processor pipeline
 */
#ifndef _ENTRY_PROC_TOOLS_H
#define _ENTRY_PROC_TOOLS_H

#include "entry_processor.h"

extern entry_proc_config_t entry_proc_conf;
extern int                 pipeline_flags;

/** initialize id constraint manager */
int            id_constraint_init( void );


#define ID_OK                   0
#define ID_CONSTRAINT_VIOLATION 1
#define ID_MISSING              2
#define ID_NOT_EXISTS           3

/**
 * This is called to register the operation (with the ordering of pipeline)
 * @return ID_OK if the entry can be processed.
 *         ID_MISSING if the ID is not set in p_op structure
 */
int            id_constraint_register( entry_proc_op_t * p_op, int at_head );


/**
 * Get the first operation for a given id.
 * @return an operation to be processed when it is possible.
 *         NULL else.
 *
 */
entry_proc_op_t *id_constraint_get_first_op( entry_id_t * p_id );


/**
 * This removes the current reference to an id when the operation is removed.
 */
int            id_constraint_unregister( entry_proc_op_t * p_op );


/* display info about id constraints management */
void id_constraint_stats(void);
/* dump all values */
void id_constraint_dump(void);

#endif
