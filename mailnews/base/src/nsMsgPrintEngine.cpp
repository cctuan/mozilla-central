/*
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "MPL"); you may not use this file
 * except in compliance with the MPL. You may obtain a copy of
 * the MPL at http://www.mozilla.org/MPL/
 * 
 * Software distributed under the MPL is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the MPL for the specific language governing
 * rights and limitations under the MPL.
 *
 */

// nsMsgPrintEngine.cpp: provides a WebShell container for use 
// in printing

#include "nscore.h"
#include "nsCOMPtr.h"

#include "nsRepository.h"

#include "nsISupports.h"

#include "nsIURI.h"
#include "nsEscape.h"
#include "nsXPIDLString.h"
#include "nsIWebShell.h"
#include "nsIDOMDocument.h"
#include "nsIDocumentViewer.h"
#include "nsIPresContext.h"
#include "nsIPresShell.h"
#include "nsIDocument.h"
#include "nsIContentViewerFile.h"
#include "nsIContentViewer.h"
#include "nsIMsgMessageService.h"
#include "nsMsgUtils.h"
#include "nsIDocumentLoader.h"
#include "nsIDocumentLoaderObserver.h"
#include "nsIMarkupDocumentViewer.h"
#include "nsIMsgMailSession.h"
#include "nsMsgPrintEngine.h"
#include "nsMsgBaseCID.h"

// Interfaces Needed
#include "nsIBaseWindow.h"
#include "nsIDocShell.h"
#include "nsIDocShellTreeItem.h"
#include "nsIDocShellTreeNode.h"
#include "nsIWebNavigation.h"

/////////////////////////////////////////////////////////////////////////
// nsMsgPrintEngine implementation
/////////////////////////////////////////////////////////////////////////

static NS_DEFINE_CID(kMsgMailSessionCID, NS_MSGMAILSESSION_CID);
static NS_DEFINE_CID(kStringBundleServiceCID, NS_STRINGBUNDLESERVICE_CID);

nsMsgPrintEngine::nsMsgPrintEngine() :
  mWebShell(nsnull),
  mWindow(nsnull)
{
  mCurrentlyPrintingURI = -1;

  NS_INIT_REFCNT();
}


nsMsgPrintEngine::~nsMsgPrintEngine()
{
}

// Implement AddRef and Release
NS_IMPL_ADDREF(nsMsgPrintEngine)
NS_IMPL_RELEASE(nsMsgPrintEngine)

NS_IMPL_QUERY_INTERFACE3(nsMsgPrintEngine, nsIMsgPrintEngine, nsIDocumentLoaderObserver, nsIPrintListener);

nsresult nsMsgPrintEngine::Init()
{
	return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::OnStartDocumentLoad(nsIDocumentLoader *aLoader, nsIURI *aURL, const char *aCommand)
{
  // Tell the user we are loading...
  PRUnichar *msg = GetString(nsString("LoadingMessageToPrint").GetUnicode());
  SetStatusMessage( msg );
  PR_FREEIF(msg);

  return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::OnEndDocumentLoad(nsIDocumentLoader *loader, nsIChannel *aChannel, PRUint32 aStatus)
{
  // Now, fire off the print operation!
  nsresult rv = NS_ERROR_FAILURE;
  nsCOMPtr<nsIContentViewer> viewer;

  // Tell the user the message is loaded...
  PRUnichar *msg = GetString(nsString("MessageLoaded").GetUnicode());
  SetStatusMessage( msg );
  PR_FREEIF(msg);

  nsCOMPtr<nsIDocShell> docShell(do_QueryInterface(mWebShell));
  NS_ASSERTION(docShell,"can't print, there is no webshell");
  if ( (!docShell) || (!aChannel) ) 
  {
    return StartNextPrintOperation();
  }

  // Make sure this isn't just "about:blank" finishing....
  nsCOMPtr<nsIURI> originalURI = nsnull;
  if (NS_SUCCEEDED(aChannel->GetOriginalURI(getter_AddRefs(originalURI))))
  {
    nsXPIDLCString spec;

    if (NS_SUCCEEDED(originalURI->GetSpec(getter_Copies(spec))) && spec)
    {      
      if (!nsCRT::strcasecmp(spec, "about:blank"))
      {
        return StartNextPrintOperation();
      }
    }
  }

  docShell->GetContentViewer(getter_AddRefs(viewer));  
  if (viewer) 
  {
    nsCOMPtr<nsIContentViewerFile> viewerFile = do_QueryInterface(viewer);
    if (viewerFile) 
    {
      if (mCurrentlyPrintingURI == 0)
        rv = viewerFile->Print(PR_FALSE, nsnull, (nsIPrintListener *)this);
      else
        rv = viewerFile->Print(PR_TRUE, nsnull, (nsIPrintListener *)this);

      // Tell the user we started printing...
      PRUnichar *msg = GetString(nsString("PrintingMessage").GetUnicode());
      SetStatusMessage( msg );
      PR_FREEIF(msg);
    }
  }

  return rv;
}

NS_IMETHODIMP
nsMsgPrintEngine::OnStartURLLoad(nsIDocumentLoader *aLoader, nsIChannel *channel)
{
  return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::OnProgressURLLoad(nsIDocumentLoader *aLoader, nsIChannel *aChannel, PRUint32 aProgress, PRUint32 aProgressMax)
{
  return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::OnStatusURLLoad(nsIDocumentLoader *loader, nsIChannel *channel, nsString & aMsg)
{
  return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::OnEndURLLoad(nsIDocumentLoader *aLoader, nsIChannel *aChannel, PRUint32 aStatus)
{
  return NS_OK;
}

NS_IMETHODIMP    
nsMsgPrintEngine::SetWindow(nsIDOMWindow *aWin)
{
	if (!aWin)
  {
    // It isn't an error to pass in null for aWin, in fact it means we are shutting
    // down and we should start cleaning things up...
		return NS_OK;
  }

  mWindow = aWin;
  nsAutoString  webShellName("printengine");

  nsCOMPtr<nsIScriptGlobalObject> globalObj( do_QueryInterface(aWin) );
  NS_ENSURE_TRUE(globalObj, NS_ERROR_FAILURE);

  nsCOMPtr<nsIDocShell> docShell;
  globalObj->GetDocShell(getter_AddRefs(docShell));

  nsCOMPtr<nsIDocShellTreeItem> docShellAsItem(do_QueryInterface(docShell));
  NS_ENSURE_TRUE(docShellAsItem, NS_ERROR_FAILURE);

  nsCOMPtr<nsIDocShellTreeItem> rootAsItem;
  docShellAsItem->GetSameTypeRootTreeItem(getter_AddRefs(rootAsItem));

  nsAutoString childName("printengine");
  nsCOMPtr<nsIDocShellTreeNode> rootAsNode(do_QueryInterface(rootAsItem));
  NS_ENSURE_TRUE(rootAsNode, NS_ERROR_FAILURE);

  nsCOMPtr<nsIDocShellTreeItem> childItem;
  rootAsNode->FindChildWithName(childName.GetUnicode(), PR_TRUE, PR_FALSE, nsnull,
    getter_AddRefs(childItem));

  mWebShell = do_QueryInterface(childItem);

  if(mWebShell)
    SetupObserver();

  return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::AddPrintURI(const PRUnichar *aMsgURI)
{
  mURIArray.AppendString(aMsgURI);
  return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::SetPrintURICount(PRInt32 aCount)
{
  mURICount = aCount;
  return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::StartPrintOperation()
{
  return StartNextPrintOperation();
}

NS_IMETHODIMP
nsMsgPrintEngine::StartNextPrintOperation()
{
  nsresult      rv;

  // Only do this the first time through...
  if (mCurrentlyPrintingURI == -1)
    InitializeDisplayCharset();

  mCurrentlyPrintingURI++;

  // First, check if we are at the end of this stuff!
  if ( mCurrentlyPrintingURI >= mURIArray.Count() )
  {
    // This is the end...dum, dum, dum....my only friend...the end
    mWindow->Close();

    // Tell the user we are done...
    PRUnichar *msg = GetString(nsString("PrintingComplete").GetUnicode());
    SetStatusMessage( msg );
    PR_FREEIF(msg);
    
    return NS_OK;
  }

  if (!mWebShell)
    return StartNextPrintOperation();

  nsString *uri = mURIArray.StringAt(mCurrentlyPrintingURI);
  rv = FireThatLoadOperation(uri);
  if (NS_FAILED(rv))
    return StartNextPrintOperation();
  else
    return rv;
}

NS_IMETHODIMP    
nsMsgPrintEngine::SetStatusFeedback(nsIMsgStatusFeedback *aFeedback)
{
	mFeedback = aFeedback;
	return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::FireThatLoadOperation(nsString *uri)
{
  nsresult      rv = NS_OK;

  char  *tString = uri->ToNewCString();
  if (!tString)
    return NS_ERROR_OUT_OF_MEMORY;

  nsIMsgMessageService * messageService = nsnull;
  rv = GetMessageServiceFromURI(tString, &messageService);
  
  if (NS_SUCCEEDED(rv) && messageService)
  {
    rv = messageService->DisplayMessageForPrinting(tString, mWebShell, nsnull, nsnull, nsnull);
    ReleaseMessageServiceFromURI(tString, messageService);
  }
  //If it's not something we know about, then just load try loading it directly.
  else
  {
    nsCOMPtr<nsIWebNavigation> webNav(do_QueryInterface(mWebShell));
    if (webNav)
      webNav->LoadURI(uri->GetUnicode());
  }

  PR_FREEIF(tString);
  return rv;
}

void
nsMsgPrintEngine::InitializeDisplayCharset()
{
  // libmime always converts to UTF-8 (both HTML and XML)
  nsCOMPtr<nsIDocShell> docShell(do_QueryInterface(mWebShell));
  if (docShell) 
  {
    nsAutoString aForceCharacterSet("UTF-8");
    nsCOMPtr<nsIContentViewer> cv;
    docShell->GetContentViewer(getter_AddRefs(cv));
    if (cv) 
    {
      nsCOMPtr<nsIMarkupDocumentViewer> muDV = do_QueryInterface(cv);
      if (muDV) 
      {
        muDV->SetForceCharacterSet(aForceCharacterSet.GetUnicode());
      }
    }
  }
}

void
nsMsgPrintEngine::SetupObserver()
{
  if (!mWebShell)
    return;

  nsCOMPtr<nsIDocShell> docShell(do_QueryInterface(mWebShell));
  if (docShell)
  {
    docShell->SetDocLoaderObserver((nsIDocumentLoaderObserver *)this);
  }
}

NS_IMETHODIMP
nsMsgPrintEngine::OnStartPrinting(void)
{
  return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::OnProgressPrinting(PRUint32 aProgress, PRUint32 aProgressMax)
{
  return NS_OK;
}

NS_IMETHODIMP
nsMsgPrintEngine::OnEndPrinting(PRUint32 aStatus)
{
  StartNextPrintOperation();
  return NS_OK;
}

nsresult
nsMsgPrintEngine::SetStatusMessage(PRUnichar *aMsgString)
{
  PRUnichar     *progressMsg;

  if ( (!mFeedback) || (!aMsgString) )
    return NS_OK;

  progressMsg = nsCRT::strdup(aMsgString);
  mFeedback->ShowStatusString(progressMsg);
  return NS_OK;
}

#define MESSENGER_STRING_URL       "chrome://messenger/locale/messenger.properties"

PRUnichar *
nsMsgPrintEngine::GetString(const PRUnichar *aStringName)
{
	nsresult    res = NS_OK;
  PRUnichar   *ptrv = nsnull;

	if (!mStringBundle)
	{
		char    *propertyURL = MESSENGER_STRING_URL;

		NS_WITH_SERVICE(nsIStringBundleService, sBundleService, kStringBundleServiceCID, &res); 
		if (NS_SUCCEEDED(res) && (nsnull != sBundleService)) 
		{
			nsILocale   *locale = nsnull;
			res = sBundleService->CreateBundle(propertyURL, locale, getter_AddRefs(mStringBundle));
		}
	}

	if (mStringBundle)
		res = mStringBundle->GetStringFromName(aStringName, &ptrv);

  if ( NS_SUCCEEDED(res) && (ptrv) )
    return ptrv;
  else
    return nsCRT::strdup(aStringName);
}
