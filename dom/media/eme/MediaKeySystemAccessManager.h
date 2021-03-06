/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MediaKeySystemAccessManager_h
#define mozilla_dom_MediaKeySystemAccessManager_h

#include "mozilla/dom/MediaKeySystemAccess.h"
#include "nsIObserver.h"
#include "nsCycleCollectionParticipant.h"
#include "nsISupportsImpl.h"
#include "nsITimer.h"

namespace mozilla {
namespace dom {

class DetailedPromise;

class MediaKeySystemAccessManager final : public nsIObserver
{
public:

  explicit MediaKeySystemAccessManager(nsPIDOMWindow* aWindow);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_CLASS_AMBIGUOUS(MediaKeySystemAccessManager, nsIObserver)
  NS_DECL_NSIOBSERVER

  void Request(DetailedPromise* aPromise,
               const nsAString& aKeySystem,
               const Optional<Sequence<MediaKeySystemOptions>>& aOptions);

  void Shutdown();

  struct PendingRequest {
    PendingRequest(DetailedPromise* aPromise,
      const nsAString& aKeySystem,
      const Sequence<MediaKeySystemOptions>& aOptions,
      nsITimer* aTimer);
    PendingRequest(const PendingRequest& aOther);
    ~PendingRequest();
    void CancelTimer();
    void RejectPromise(const nsCString& aReason);

    nsRefPtr<DetailedPromise> mPromise;
    const nsString mKeySystem;
    const Sequence<MediaKeySystemOptions> mOptions;
    nsCOMPtr<nsITimer> mTimer;
  };

private:

  enum RequestType {
    Initial,
    Subsequent
  };

  void Request(DetailedPromise* aPromise,
               const nsAString& aKeySystem,
               const Sequence<MediaKeySystemOptions>& aOptions,
               RequestType aType);

  ~MediaKeySystemAccessManager();

  bool EnsureObserversAdded();

  bool AwaitInstall(DetailedPromise* aPromise,
                    const nsAString& aKeySystem,
                    const Sequence<MediaKeySystemOptions>& aOptions);

  void RetryRequest(PendingRequest& aRequest);

  nsTArray<PendingRequest> mRequests;

  nsCOMPtr<nsPIDOMWindow> mWindow;
  bool mAddedObservers;
};

} // namespace dom
} // namespace mozilla

#endif
