/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

/* nsDll
 *
 * Abstraction of a Dll. Stores modifiedTime and size for easy detection of
 * change in dll.
 *
 * dp Suresh <dp@netscape.com>
 */

#include "xcDll.h"
#include "plstr.h"	// strdup and strfree

nsDll::nsDll(const char *libFullPath) : m_fullpath(NULL), m_instance(NULL),
	m_status(DLL_OK)
{
	// XXX No initializer for PRTime's
	// m_lastModTime = 0;
	LL_I2L(m_size, 0);
	if (libFullPath == NULL)
	{
		m_status = DLL_INVALID_PARAM;
		return;
	}
	m_fullpath = PL_strdup(libFullPath);
	if (m_fullpath == NULL)
	{
		// No more memory
		m_status = DLL_NO_MEM;
		return;
	}

	PRFileInfo64 statinfo;
	if (PR_GetFileInfo64(m_fullpath, &statinfo) != PR_SUCCESS)
	{
		// The stat things works only if people pass in the full pathname.
		// Even if our stat fails, we could be able to load it because of
		// LD_LIBRARY_PATH and other such paths where dlls are searched for

		// XXX we need a way of marking this occurance.
		// XXX m_status = DLL_STAT_ERROR;
	}
	else 
	{
		m_size = statinfo.size;
		m_lastModTime = statinfo.modifyTime;
		if (statinfo.type != PR_FILE_FILE)
		{
			// Not a file. Cant work with it.
			m_status = DLL_NOT_FILE;
			return;
		}
	}
	m_status = DLL_OK;			
}

nsDll::~nsDll(void)
{
	if (m_instance != NULL)
		Unload();
	if (m_fullpath != NULL) PL_strfree(m_fullpath);
	m_fullpath = NULL;
}

PRBool nsDll::Load(void)
{
	if (m_status != DLL_OK)
	{
		return (PR_FALSE);
	}
	if (m_instance != NULL)
	{
		// Already loaded
		return (PR_TRUE);
	}
	m_instance = PR_LoadLibrary(m_fullpath);
	return ((m_instance == NULL) ? PR_FALSE : PR_TRUE);
	
}

PRBool nsDll::Unload(void)
{
	if (m_status != DLL_OK || m_instance == NULL)
		return (PR_FALSE);
	PRStatus ret = PR_UnloadLibrary(m_instance);
	if (ret == PR_SUCCESS)
	{
		m_instance = NULL;
		return (PR_TRUE);
	}
	else
		return (PR_FALSE);
}

void * nsDll::FindSymbol(const char *symbol)
{
	if (symbol == NULL)
		return (NULL);
	
	// If not already loaded, load it now.
	if (Load() != PR_TRUE)
		return (NULL);

	return (PR_FindSymbol(m_instance, symbol));
}
