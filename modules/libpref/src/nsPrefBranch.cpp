/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Alec Flett <alecf@netscape.com>
 *   Brian Nesse <bnesse@netscape.com>
 *   Frederic Plourde <frederic.plourde@collabora.co.uk>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifdef MOZ_IPC
#include "mozilla/dom/ContentChild.h"
#include "nsXULAppAPI.h"
#endif

#include "nsPrefBranch.h"
#include "nsPrefService.h"
#include "nsILocalFile.h"
#include "nsIObserverService.h"
#include "nsXPCOM.h"
#include "nsISupportsPrimitives.h"
#include "nsIDirectoryService.h"
#include "nsString.h"
#include "nsReadableUtils.h"
#include "nsXPIDLString.h"
#include "nsThreadUtils.h"
#include "nsIStringBundle.h"
#include "prefapi.h"
#include "prmem.h"
#include "pldhash.h"

#include "plstr.h"
#include "nsCRT.h"
#include "mozilla/Services.h"

#include "prefapi_private_data.h"

// Definitions
struct EnumerateData {
  const char  *parent;
  nsTArray<nsCString> *pref_list;
};

struct PrefCallbackData {
  nsPrefBranch     *pBranch;
  nsISupports      *pCanonical;
  nsIObserver      *pObserver;
  nsIWeakReference *pWeakRef;
  char pDomain[1];
};


// Prototypes
static PLDHashOperator
  pref_enumChild(PLDHashTable *table, PLDHashEntryHdr *heh,
                 PRUint32 i, void *arg);
static nsresult
  NotifyObserver(const char *newpref, void *data);

#ifdef MOZ_IPC
using mozilla::dom::ContentChild;

static ContentChild*
GetContentChild()
{
  if (XRE_GetProcessType() == GeckoProcessType_Content) {
    ContentChild* cpc = ContentChild::GetSingleton();
    if (!cpc) {
      NS_RUNTIMEABORT("Content Protocol is NULL!  We're going to crash!");
    }
    return cpc;
  }
  return nsnull;
}
#endif  // MOZ_IPC

/*
 * Constructor/Destructor
 */

nsPrefBranch::nsPrefBranch(const char *aPrefRoot, PRBool aDefaultBranch)
  : mObservers(nsnull)
{
  mPrefRoot = aPrefRoot;
  mPrefRootLength = mPrefRoot.Length();
  mIsDefault = aDefaultBranch;

  nsCOMPtr<nsIObserverService> observerService =
    mozilla::services::GetObserverService();
  if (observerService) {
    ++mRefCnt;    // Our refcnt must be > 0 when we call this, or we'll get deleted!
    // add weak so we don't have to clean up at shutdown
    observerService->AddObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID, PR_TRUE);
    --mRefCnt;
  }
}

nsPrefBranch::~nsPrefBranch()
{
  freeObserverList();
}


/*
 * nsISupports Implementation
 */

NS_IMPL_THREADSAFE_ADDREF(nsPrefBranch)
NS_IMPL_THREADSAFE_RELEASE(nsPrefBranch)

NS_INTERFACE_MAP_BEGIN(nsPrefBranch)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIPrefBranch)
  NS_INTERFACE_MAP_ENTRY(nsIPrefBranch)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIPrefBranch2, !mIsDefault)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIPrefBranchInternal, !mIsDefault)
  NS_INTERFACE_MAP_ENTRY(nsIObserver)
  NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
NS_INTERFACE_MAP_END


/*
 * nsIPrefBranch Implementation
 */

NS_IMETHODIMP nsPrefBranch::GetRoot(char **aRoot)
{
  NS_ENSURE_ARG_POINTER(aRoot);
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

  mPrefRoot.Truncate(mPrefRootLength);
  *aRoot = ToNewCString(mPrefRoot);
  return NS_OK;
}

NS_IMETHODIMP nsPrefBranch::GetPrefType(const char *aPrefName, PRInt32 *_retval)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (ContentChild* cpc = GetContentChild()) {
    nsresult rv = NS_ERROR_NOT_AVAILABLE;
    PRInt32 retval;
    cpc->SendGetPrefType(nsDependentCString(getPrefName(aPrefName)), &retval, &rv);
    if (NS_SUCCEEDED(rv))
      *_retval = retval;
    return rv;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  *_retval = PREF_GetPrefType(pref);
  return NS_OK;
}

NS_IMETHODIMP nsPrefBranch::GetBoolPref(const char *aPrefName, PRBool *_retval)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (ContentChild* cpc = GetContentChild()) {
    nsresult rv = NS_ERROR_NOT_AVAILABLE;
    PRBool retval;
    cpc->SendGetBoolPref(nsDependentCString(getPrefName(aPrefName)), &retval, &rv);
    if (NS_SUCCEEDED(rv))
      *_retval = retval;
    return rv;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  return PREF_GetBoolPref(pref, _retval, mIsDefault);
}

NS_IMETHODIMP nsPrefBranch::SetBoolPref(const char *aPrefName, PRInt32 aValue)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (GetContentChild()) {
    NS_ERROR("cannot set pref from content process");
    return NS_ERROR_NOT_AVAILABLE;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  return PREF_SetBoolPref(pref, aValue, mIsDefault);
}

NS_IMETHODIMP nsPrefBranch::GetCharPref(const char *aPrefName, char **_retval)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (ContentChild* cpc = GetContentChild()) {
    nsresult rv = NS_ERROR_NOT_AVAILABLE;
    nsCAutoString prefValue;
    cpc->SendGetCharPref(nsDependentCString(getPrefName(aPrefName)), 
                         &prefValue, &rv);
    if (NS_SUCCEEDED(rv)) {
      *_retval = strdup(prefValue.get());
    }
    return rv;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  return PREF_CopyCharPref(pref, _retval, mIsDefault);
}

NS_IMETHODIMP nsPrefBranch::SetCharPref(const char *aPrefName, const char *aValue)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (GetContentChild()) {
    NS_ERROR("cannot set pref from content process");
    return NS_ERROR_NOT_AVAILABLE;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  return PREF_SetCharPref(pref, aValue, mIsDefault);
}

NS_IMETHODIMP nsPrefBranch::GetIntPref(const char *aPrefName, PRInt32 *_retval)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (ContentChild* cpc = GetContentChild()) {
    nsresult rv = NS_ERROR_NOT_AVAILABLE;
    PRInt32 retval;
    cpc->SendGetIntPref(nsDependentCString(getPrefName(aPrefName)), &retval, &rv);
    if (NS_SUCCEEDED(rv))
      *_retval = retval;
    return rv;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  return PREF_GetIntPref(pref, _retval, mIsDefault);
}

NS_IMETHODIMP nsPrefBranch::SetIntPref(const char *aPrefName, PRInt32 aValue)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (GetContentChild()) {
    NS_ERROR("cannot set pref from content process");
    return NS_ERROR_NOT_AVAILABLE;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  return PREF_SetIntPref(pref, aValue, mIsDefault);
}

NS_IMETHODIMP nsPrefBranch::GetComplexValue(const char *aPrefName, const nsIID & aType, void **_retval)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

  nsresult       rv;
  nsXPIDLCString utf8String;

  // we have to do this one first because it's different than all the rest
  if (aType.Equals(NS_GET_IID(nsIPrefLocalizedString))) {
    nsCOMPtr<nsIPrefLocalizedString> theString(do_CreateInstance(NS_PREFLOCALIZEDSTRING_CONTRACTID, &rv));
    if (NS_FAILED(rv)) return rv;

#ifdef MOZ_IPC
    if (ContentChild* cpc = GetContentChild()) {
      nsAutoString prefValue;

      rv = NS_ERROR_NOT_AVAILABLE;
      cpc->SendGetPrefLocalizedString(nsDependentCString(getPrefName(aPrefName)), 
                                      &prefValue, &rv);
      if (NS_FAILED(rv)) return rv;

      theString->SetData(prefValue.get());
      theString.forget(reinterpret_cast<nsIPrefLocalizedString**>(_retval));
      return rv;
    }
#endif

    const char *pref = getPrefName(aPrefName);
    PRBool  bNeedDefault = PR_FALSE;

    if (mIsDefault) {
      bNeedDefault = PR_TRUE;
    } else {
      // if there is no user (or locked) value
      if (!PREF_HasUserPref(pref) && !PREF_PrefIsLocked(pref)) {
        bNeedDefault = PR_TRUE;
      }
    }

    // if we need to fetch the default value, do that instead, otherwise use the
    // value we pulled in at the top of this function
    if (bNeedDefault) {
      nsXPIDLString utf16String;
      rv = GetDefaultFromPropertiesFile(pref, getter_Copies(utf16String));
      if (NS_SUCCEEDED(rv)) {
        theString->SetData(utf16String.get());
      }
    } else {
      rv = GetCharPref(aPrefName, getter_Copies(utf8String));
      if (NS_SUCCEEDED(rv)) {
        theString->SetData(NS_ConvertUTF8toUTF16(utf8String).get());
      }
    }

    if (NS_SUCCEEDED(rv)) {
      const char *pref = getPrefName(aPrefName);
      PRBool  bNeedDefault = PR_FALSE;

      if (mIsDefault) {
        bNeedDefault = PR_TRUE;
      } else {
        // if there is no user (or locked) value
        if (!PREF_HasUserPref(pref) && !PREF_PrefIsLocked(pref)) {
          bNeedDefault = PR_TRUE;
        }
      }

      // if we need to fetch the default value, do that instead, otherwise use the
      // value we pulled in at the top of this function
      if (bNeedDefault) {
        nsXPIDLString utf16String;
        rv = GetDefaultFromPropertiesFile(pref, getter_Copies(utf16String));
        if (NS_SUCCEEDED(rv)) {
          rv = theString->SetData(utf16String.get());
        }
      } else {
        rv = GetCharPref(aPrefName, getter_Copies(utf8String));
        if (NS_SUCCEEDED(rv)) {
          rv = theString->SetData(NS_ConvertUTF8toUTF16(utf8String).get());
        }
      }
      if (NS_SUCCEEDED(rv)) {
        nsIPrefLocalizedString *temp = theString;

        NS_ADDREF(temp);
        *_retval = (void *)temp;
      }
    }

    return rv;
  }

  // if we can't get the pref, there's no point in being here
  rv = GetCharPref(aPrefName, getter_Copies(utf8String));
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (aType.Equals(NS_GET_IID(nsILocalFile))) {
#ifdef MOZ_IPC
    if (GetContentChild()) {
      NS_ERROR("cannot get nsILocalFile pref from content process");
      return NS_ERROR_NOT_AVAILABLE;
    }
#endif

    nsCOMPtr<nsILocalFile> file(do_CreateInstance(NS_LOCAL_FILE_CONTRACTID, &rv));

    if (NS_SUCCEEDED(rv)) {
      rv = file->SetPersistentDescriptor(utf8String);
      if (NS_SUCCEEDED(rv)) {
        file.forget(reinterpret_cast<nsILocalFile**>(_retval));
        return NS_OK;
      }
    }
    return rv;
  }

  if (aType.Equals(NS_GET_IID(nsIRelativeFilePref))) {
#ifdef MOZ_IPC
    if (GetContentChild()) {
      NS_ERROR("cannot get nsIRelativeFilePref from content process");
      return NS_ERROR_NOT_AVAILABLE;
    }
#endif

    nsACString::const_iterator keyBegin, strEnd;
    utf8String.BeginReading(keyBegin);
    utf8String.EndReading(strEnd);    

    // The pref has the format: [fromKey]a/b/c
    if (*keyBegin++ != '[')        
      return NS_ERROR_FAILURE;
    nsACString::const_iterator keyEnd(keyBegin);
    if (!FindCharInReadable(']', keyEnd, strEnd))
      return NS_ERROR_FAILURE;
    nsCAutoString key(Substring(keyBegin, keyEnd));
    
    nsCOMPtr<nsILocalFile> fromFile;        
    nsCOMPtr<nsIProperties> directoryService(do_GetService(NS_DIRECTORY_SERVICE_CONTRACTID, &rv));
    if (NS_FAILED(rv))
      return rv;
    rv = directoryService->Get(key.get(), NS_GET_IID(nsILocalFile), getter_AddRefs(fromFile));
    if (NS_FAILED(rv))
      return rv;
    
    nsCOMPtr<nsILocalFile> theFile;
    rv = NS_NewNativeLocalFile(EmptyCString(), PR_TRUE, getter_AddRefs(theFile));
    if (NS_FAILED(rv))
      return rv;
    rv = theFile->SetRelativeDescriptor(fromFile, Substring(++keyEnd, strEnd));
    if (NS_FAILED(rv))
      return rv;
    nsCOMPtr<nsIRelativeFilePref> relativePref;
    rv = NS_NewRelativeFilePref(theFile, key, getter_AddRefs(relativePref));
    if (NS_FAILED(rv))
      return rv;

    relativePref.forget(reinterpret_cast<nsIRelativeFilePref**>(_retval));
    return NS_OK;
  }

  if (aType.Equals(NS_GET_IID(nsISupportsString))) {
    nsCOMPtr<nsISupportsString> theString(do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID, &rv));

    if (NS_SUCCEEDED(rv)) {
      theString->SetData(NS_ConvertUTF8toUTF16(utf8String));
      theString.forget(reinterpret_cast<nsISupportsString**>(_retval));
    }
    return rv;
  }

  NS_WARNING("nsPrefBranch::GetComplexValue - Unsupported interface type");
  return NS_NOINTERFACE;
}

NS_IMETHODIMP nsPrefBranch::SetComplexValue(const char *aPrefName, const nsIID & aType, nsISupports *aValue)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (GetContentChild()) {
    NS_ERROR("cannot set pref from content process");
    return NS_ERROR_NOT_AVAILABLE;
  }
#endif

  nsresult   rv = NS_NOINTERFACE;

  if (aType.Equals(NS_GET_IID(nsILocalFile))) {
    nsCOMPtr<nsILocalFile> file = do_QueryInterface(aValue);
    if (!file)
      return NS_NOINTERFACE;
    nsCAutoString descriptorString;

    rv = file->GetPersistentDescriptor(descriptorString);
    if (NS_SUCCEEDED(rv)) {
      rv = SetCharPref(aPrefName, descriptorString.get());
    }
    return rv;
  }

  if (aType.Equals(NS_GET_IID(nsIRelativeFilePref))) {
    nsCOMPtr<nsIRelativeFilePref> relFilePref = do_QueryInterface(aValue);
    if (!relFilePref)
      return NS_NOINTERFACE;
    
    nsCOMPtr<nsILocalFile> file;
    relFilePref->GetFile(getter_AddRefs(file));
    if (!file)
      return NS_NOINTERFACE;
    nsCAutoString relativeToKey;
    (void) relFilePref->GetRelativeToKey(relativeToKey);

    nsCOMPtr<nsILocalFile> relativeToFile;        
    nsCOMPtr<nsIProperties> directoryService(do_GetService(NS_DIRECTORY_SERVICE_CONTRACTID, &rv));
    if (NS_FAILED(rv))
      return rv;
    rv = directoryService->Get(relativeToKey.get(), NS_GET_IID(nsILocalFile), getter_AddRefs(relativeToFile));
    if (NS_FAILED(rv))
      return rv;

    nsCAutoString relDescriptor;
    rv = file->GetRelativeDescriptor(relativeToFile, relDescriptor);
    if (NS_FAILED(rv))
      return rv;
    
    nsCAutoString descriptorString;
    descriptorString.Append('[');
    descriptorString.Append(relativeToKey);
    descriptorString.Append(']');
    descriptorString.Append(relDescriptor);
    return SetCharPref(aPrefName, descriptorString.get());
  }

  if (aType.Equals(NS_GET_IID(nsISupportsString))) {
    nsCOMPtr<nsISupportsString> theString = do_QueryInterface(aValue);

    if (theString) {
      nsAutoString wideString;

      rv = theString->GetData(wideString);
      if (NS_SUCCEEDED(rv)) {
        rv = SetCharPref(aPrefName, NS_ConvertUTF16toUTF8(wideString).get());
      }
    }
    return rv;
  }

  if (aType.Equals(NS_GET_IID(nsIPrefLocalizedString))) {
    nsCOMPtr<nsIPrefLocalizedString> theString = do_QueryInterface(aValue);

    if (theString) {
      nsXPIDLString wideString;

      rv = theString->GetData(getter_Copies(wideString));
      if (NS_SUCCEEDED(rv)) {
        rv = SetCharPref(aPrefName, NS_ConvertUTF16toUTF8(wideString).get());
      }
    }
    return rv;
  }

  NS_WARNING("nsPrefBranch::SetComplexValue - Unsupported interface type");
  return NS_NOINTERFACE;
}

NS_IMETHODIMP nsPrefBranch::ClearUserPref(const char *aPrefName)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (GetContentChild()) {
    NS_ERROR("cannot set pref from content process");
    return NS_ERROR_NOT_AVAILABLE;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  return PREF_ClearUserPref(pref);
}

NS_IMETHODIMP nsPrefBranch::PrefHasUserValue(const char *aPrefName, PRBool *_retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (ContentChild* cpc = GetContentChild()) {
    nsresult rv = NS_ERROR_NOT_AVAILABLE;
    PRBool retval;
    cpc->SendPrefHasUserValue(nsDependentCString(getPrefName(aPrefName)), &retval, &rv);
    if (NS_SUCCEEDED(rv))
      *_retval = retval;
    return rv;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  *_retval = PREF_HasUserPref(pref);
  return NS_OK;
}

NS_IMETHODIMP nsPrefBranch::LockPref(const char *aPrefName)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (GetContentChild()) {
    NS_ERROR("cannot lock pref from content process");
    return NS_ERROR_NOT_AVAILABLE;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  return PREF_LockPref(pref, PR_TRUE);
}

NS_IMETHODIMP nsPrefBranch::PrefIsLocked(const char *aPrefName, PRBool *_retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (ContentChild* cpc = GetContentChild()) {
    nsresult rv = NS_ERROR_NOT_AVAILABLE;
    PRBool retval;
    cpc->SendPrefIsLocked(nsDependentCString(getPrefName(aPrefName)), &retval, &rv);
    if (NS_SUCCEEDED(rv))
      *_retval = retval;
    return rv;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  *_retval = PREF_PrefIsLocked(pref);
  return NS_OK;
}

NS_IMETHODIMP nsPrefBranch::UnlockPref(const char *aPrefName)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (GetContentChild()) {
    NS_ERROR("cannot unlock pref from content process");
    return NS_ERROR_NOT_AVAILABLE;
  }
#endif

  const char *pref = getPrefName(aPrefName);
  return PREF_LockPref(pref, PR_FALSE);
}

/* void resetBranch (in string startingAt); */
NS_IMETHODIMP nsPrefBranch::ResetBranch(const char *aStartingAt)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP nsPrefBranch::DeleteBranch(const char *aStartingAt)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (GetContentChild()) {
    NS_ERROR("cannot set pref from content process");
    return NS_ERROR_NOT_AVAILABLE;
  }
#endif

  const char *pref = getPrefName(aStartingAt);
  return PREF_DeleteBranch(pref);
}

NS_IMETHODIMP nsPrefBranch::GetChildList(const char *aStartingAt, PRUint32 *aCount, char ***aChildArray)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

  char            **outArray;
  PRInt32         numPrefs;
  PRInt32         dwIndex;
  EnumerateData   ed;
  nsAutoTArray<nsCString, 32> prefArray;

  NS_ENSURE_ARG_POINTER(aStartingAt);
  NS_ENSURE_ARG_POINTER(aCount);
  NS_ENSURE_ARG_POINTER(aChildArray);

  *aChildArray = nsnull;
  *aCount = 0;

#ifdef MOZ_IPC
  if (ContentChild* cpc = GetContentChild()) {
    nsresult rv = NS_ERROR_NOT_AVAILABLE;
    cpc->SendGetChildList(nsDependentCString(getPrefName(aStartingAt)),
                          &prefArray, &rv);
    if (NS_FAILED(rv)) return rv;

  } else
#endif
  {
    if (!gHashTable.ops)
      return NS_ERROR_NOT_INITIALIZED;

    // this will contain a list of all the pref name strings
    // allocate on the stack for speed

    ed.parent = getPrefName(aStartingAt);
    ed.pref_list = &prefArray;
    PL_DHashTableEnumerate(&gHashTable, pref_enumChild, &ed);
  }

  // now that we've built up the list, run the callback on
  // all the matching elements
  numPrefs = prefArray.Length();

  if (numPrefs) {
    outArray = (char **)nsMemory::Alloc(numPrefs * sizeof(char *));
    if (!outArray)
      return NS_ERROR_OUT_OF_MEMORY;

    for (dwIndex = 0; dwIndex < numPrefs; ++dwIndex) {
      // we need to lop off mPrefRoot in case the user is planning to pass this
      // back to us because if they do we are going to add mPrefRoot again.
      const nsCString& element = prefArray[dwIndex];
      outArray[dwIndex] = (char *)nsMemory::Clone(
        element.get() + mPrefRootLength, element.Length() - mPrefRootLength + 1);

      if (!outArray[dwIndex]) {
        // we ran out of memory... this is annoying
        NS_FREE_XPCOM_ALLOCATED_POINTER_ARRAY(dwIndex, outArray);
        return NS_ERROR_OUT_OF_MEMORY;
      }
    }
    *aChildArray = outArray;
  }
  *aCount = numPrefs;

  return NS_OK;
}


/*
 *  nsIPrefBranch2 methods
 */

NS_IMETHODIMP nsPrefBranch::AddObserver(const char *aDomain, nsIObserver *aObserver, PRBool aHoldWeak)
{
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

  PrefCallbackData *pCallback;
  const char *pref;

  NS_ENSURE_ARG_POINTER(aDomain);
  NS_ENSURE_ARG_POINTER(aObserver);

#ifdef MOZ_IPC
  if (ContentChild* cpc = GetContentChild()) {
    return cpc->AddRemotePrefObserver(nsDependentCString(aDomain), mPrefRoot, aObserver, aHoldWeak);
  }
#endif

  if (!mObservers) {
    mObservers = new nsAutoVoidArray();
    if (nsnull == mObservers)
      return NS_ERROR_OUT_OF_MEMORY;
  }

  pCallback = (PrefCallbackData *)NS_Alloc(sizeof(PrefCallbackData) + strlen(aDomain));
  if (nsnull == pCallback)
    return NS_ERROR_OUT_OF_MEMORY;

  pCallback->pBranch = this;
  pCallback->pObserver = aObserver;

  // hold a weak reference to the observer if so requested
  if (aHoldWeak) {
    nsCOMPtr<nsISupportsWeakReference> weakRefFactory = do_QueryInterface(aObserver);
    if (!weakRefFactory) {
      // the caller didn't give us a object that supports weak reference... tell them
      nsMemory::Free(pCallback);
      return NS_ERROR_INVALID_ARG;
    }
    nsCOMPtr<nsIWeakReference> tmp = do_GetWeakReference(weakRefFactory);
    NS_ADDREF(pCallback->pWeakRef = tmp);
  } else {
    pCallback->pWeakRef = nsnull;
    NS_ADDREF(pCallback->pObserver);
  }

  // In RemoveObserver, we need a canonical pointer to compare the removed
  // observer to. In order to avoid having to call QI every time through the
  // loop, we call QI here. There are two cases:
  // 1. We hold a strong reference to the nsIObserver. This must hold
  //    pCanonical alive.
  // 2. We hold a weak reference: either the object stays alive and eventually
  //    we QI the nsIObserver to nsISupports and compare successfully. Or the
  //    object dies and we have a dangling (but unused) pointer to the
  //    canonical supports pointer until we remove this entry.
  CallQueryInterface(aObserver, &pCallback->pCanonical);
  pCallback->pCanonical->Release();

  strcpy(pCallback->pDomain, aDomain);
  mObservers->AppendElement(pCallback);

  // We must pass a fully qualified preference name to the callback
  pref = getPrefName(aDomain); // aDomain == nsnull only possible failure, trapped above
  PREF_RegisterCallback(pref, NotifyObserver, pCallback);
  return NS_OK;
}

NS_IMETHODIMP nsPrefBranch::RemoveObserver(const char *aDomain, nsIObserver *aObserver)
{
  NS_ENSURE_ARG_POINTER(aDomain);
  NS_ENSURE_ARG_POINTER(aObserver);
  if (NS_UNLIKELY(!nsPrefService::CheckAndLogBackgroundThreadUse())) {
    return NS_ERROR_UNEXPECTED;
  }

#ifdef MOZ_IPC
  if (XRE_GetProcessType() == GeckoProcessType_Content) {
    nsresult rv = NS_OK;
    ContentChild *cpc = ContentChild::GetSingleton();
    // In case cpc doesn't exist here, we're silently returning (instead of
    // asserting), because the child process is likely to be null
    // when this is called during xpcom-shutdown.
    if (cpc)
      rv = cpc->RemoveRemotePrefObserver(nsDependentCString(aDomain), 
                                         mPrefRoot, 
                                         aObserver);
    return rv;
  }
#endif

  if (!mObservers)
    return NS_OK;

  nsCOMPtr<nsISupports> canonical(do_QueryInterface(aObserver));
  return RemoveObserverFromList(aDomain, canonical);
}

NS_IMETHODIMP nsPrefBranch::Observe(nsISupports *aSubject, const char *aTopic, const PRUnichar *someData)
{
  // watch for xpcom shutdown and free our observers to eliminate any cyclic references
  if (!nsCRT::strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID)) {
    freeObserverList();
  }
  return NS_OK;
}

static nsresult NotifyObserver(const char *newpref, void *data)
{
#ifdef MOZ_IPC
  if (GetContentChild()) {
    // We shouldn't ever get here, since we never register NotifyObserver in the 
    // content process
    NS_NOTREACHED("Remote prefs observation should be done from the \
                  chrome process!");
    return NS_OK;
  }
#endif

  PrefCallbackData *pData = (PrefCallbackData *)data;

  // remove any root this string may contain so as to not confuse the observer
  // by passing them something other than what they passed us as a topic
  PRUint32 len = pData->pBranch->GetRootLength();
  nsCAutoString suffix(newpref + len);  

  nsCOMPtr<nsIObserver> observer;
  if (pData->pWeakRef) {
    observer = do_QueryReferent(pData->pWeakRef);
    if (!observer) {
      // this weak referenced observer went away, remove them from the list
      pData->pBranch->RemoveObserverFromList(pData->pDomain, pData->pCanonical);
      return NS_OK;
    }
  } else {
    observer = pData->pObserver;
  }

  observer->Observe(static_cast<nsIPrefBranch *>(pData->pBranch),
                    NS_PREFBRANCH_PREFCHANGE_TOPIC_ID,
                    NS_ConvertASCIItoUTF16(suffix).get());
  return NS_OK;
}


void nsPrefBranch::freeObserverList(void)
{
  const char *pref;
  PrefCallbackData *pCallback;

  if (mObservers) {
    // unregister the observers

    PRInt32 i;
    nsCAutoString domain;
    for (i = 0; i < mObservers->Count(); ++i) {
      pCallback = (PrefCallbackData *)mObservers->ElementAt(i);
      if (pCallback) {
        // We must pass a fully qualified preference name to remove the callback
        pref = getPrefName(pCallback->pDomain);
        // Remove this observer from our array so that nobody else can remove
        // what we're trying to remove right now.
        mObservers->ReplaceElementAt(nsnull, i);
        PREF_UnregisterCallback(pref, NotifyObserver, pCallback);
        if (pCallback->pWeakRef) {
          NS_RELEASE(pCallback->pWeakRef);
        } else {
          NS_RELEASE(pCallback->pObserver);
        }
        nsMemory::Free(pCallback);
      }
    }

    delete mObservers;
    mObservers = 0;
  }
}

nsresult
nsPrefBranch::RemoveObserverFromList(const char *aDomain, nsISupports *aObserver)
{
  PRInt32 count = mObservers->Count();
  if (!count) {
    return NS_OK;
  }

#ifdef DEBUG
  PRBool alreadyRemoved = PR_FALSE;
#endif

  for (PRInt32 i = 0; i < count; i++) {
    PrefCallbackData *pCallback = (PrefCallbackData *)mObservers->ElementAt(i);

#ifdef DEBUG
    if (!pCallback) {
      // Have to assume that the callback we're looking for was already
      // removed in freeObserverList below.
      alreadyRemoved = PR_TRUE;
    }
#endif

    if (pCallback &&
        pCallback->pCanonical == aObserver &&
        !strcmp(pCallback->pDomain, aDomain)) {
      // We must pass a fully qualified preference name to remove the callback
      const char *pref = getPrefName(aDomain); // aDomain == nsnull only possible failure, trapped above
      nsresult rv = PREF_UnregisterCallback(pref, NotifyObserver, pCallback);
      if (NS_SUCCEEDED(rv)) {
        // Remove this observer from our array so that nobody else can remove
        // what we're trying to remove ourselves right now.
        mObservers->RemoveElementAt(i);
        if (pCallback->pWeakRef) {
          NS_RELEASE(pCallback->pWeakRef);
        } else {
          NS_RELEASE(pCallback->pObserver);
        }
        NS_Free(pCallback);
      }
      return rv;
    }
  }

  NS_WARN_IF_FALSE(alreadyRemoved,
                   "Failed attempt to remove a pref observer, probably leaking");
  return NS_OK;
}
 
nsresult nsPrefBranch::GetDefaultFromPropertiesFile(const char *aPrefName, PRUnichar **return_buf)
{
  nsresult rv;

  // the default value contains a URL to a .properties file
    
  nsXPIDLCString propertyFileURL;
  rv = PREF_CopyCharPref(aPrefName, getter_Copies(propertyFileURL), PR_TRUE);
  if (NS_FAILED(rv))
    return rv;

  nsCOMPtr<nsIStringBundleService> bundleService =
    mozilla::services::GetStringBundleService();
  if (!bundleService)
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsIStringBundle> bundle;
  rv = bundleService->CreateBundle(propertyFileURL,
                                   getter_AddRefs(bundle));
  if (NS_FAILED(rv))
    return rv;

  // string names are in unicode
  nsAutoString stringId;
  stringId.AssignASCII(aPrefName);

  return bundle->GetStringFromName(stringId.get(), return_buf);
}

const char *nsPrefBranch::getPrefName(const char *aPrefName)
{
  // for speed, avoid strcpy if we can:
  if (mPrefRoot.IsEmpty())
    return aPrefName;

  // isn't there a better way to do this? this is really kind of gross.
  mPrefRoot.Truncate(mPrefRootLength);

  // only append if anything to append
  if ((nsnull != aPrefName) && (*aPrefName != '\0'))
    mPrefRoot.Append(aPrefName);

  return mPrefRoot.get();
}

static PLDHashOperator
pref_enumChild(PLDHashTable *table, PLDHashEntryHdr *heh,
               PRUint32 i, void *arg)
{
  PrefHashEntry *he = static_cast<PrefHashEntry*>(heh);
  EnumerateData *d = reinterpret_cast<EnumerateData *>(arg);
  if (strncmp(he->key, d->parent, strlen(d->parent)) == 0) {
    d->pref_list->AppendElement(he->key);
  }
  return PL_DHASH_NEXT;
}

//----------------------------------------------------------------------------
// nsPrefLocalizedString
//----------------------------------------------------------------------------

nsPrefLocalizedString::nsPrefLocalizedString()
{
}

nsPrefLocalizedString::~nsPrefLocalizedString()
{
}


/*
 * nsISupports Implementation
 */

NS_IMPL_THREADSAFE_ADDREF(nsPrefLocalizedString)
NS_IMPL_THREADSAFE_RELEASE(nsPrefLocalizedString)

NS_INTERFACE_MAP_BEGIN(nsPrefLocalizedString)
    NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIPrefLocalizedString)
    NS_INTERFACE_MAP_ENTRY(nsIPrefLocalizedString)
    NS_INTERFACE_MAP_ENTRY(nsISupportsString)
NS_INTERFACE_MAP_END

nsresult nsPrefLocalizedString::Init()
{
  nsresult rv;
  mUnicodeString = do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID, &rv);

  return rv;
}

NS_IMETHODIMP
nsPrefLocalizedString::GetData(PRUnichar **_retval)
{
  nsAutoString data;

  nsresult rv = GetData(data);
  if (NS_FAILED(rv))
    return rv;
  
  *_retval = ToNewUnicode(data);
  if (!*_retval)
    return NS_ERROR_OUT_OF_MEMORY;

  return NS_OK;
}

NS_IMETHODIMP
nsPrefLocalizedString::SetData(const PRUnichar *aData)
{
  if (!aData)
    return SetData(EmptyString());
  return SetData(nsDependentString(aData));
}

NS_IMETHODIMP
nsPrefLocalizedString::SetDataWithLength(PRUint32 aLength,
                                         const PRUnichar *aData)
{
  if (!aData)
    return SetData(EmptyString());
  return SetData(Substring(aData, aData + aLength));
}

//----------------------------------------------------------------------------
// nsRelativeFilePref
//----------------------------------------------------------------------------

NS_IMPL_THREADSAFE_ISUPPORTS1(nsRelativeFilePref, nsIRelativeFilePref)

nsRelativeFilePref::nsRelativeFilePref()
{
}

nsRelativeFilePref::~nsRelativeFilePref()
{
}

NS_IMETHODIMP nsRelativeFilePref::GetFile(nsILocalFile **aFile)
{
  NS_ENSURE_ARG_POINTER(aFile);
  *aFile = mFile;
  NS_IF_ADDREF(*aFile);
  return NS_OK;
}

NS_IMETHODIMP nsRelativeFilePref::SetFile(nsILocalFile *aFile)
{
  mFile = aFile;
  return NS_OK;
}

NS_IMETHODIMP nsRelativeFilePref::GetRelativeToKey(nsACString& aRelativeToKey)
{
  aRelativeToKey.Assign(mRelativeToKey);
  return NS_OK;
}

NS_IMETHODIMP nsRelativeFilePref::SetRelativeToKey(const nsACString& aRelativeToKey)
{
  mRelativeToKey.Assign(aRelativeToKey);
  return NS_OK;
}
