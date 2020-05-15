/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsCSPService_h___
#define nsCSPService_h___

#include "nsXPCOM.h"
#include "nsIContentPolicy.h"
#include "nsIChannel.h"
#include "nsIChannelEventSink.h"
#include "nsDataHashtable.h"

#define CSPSERVICE_CONTRACTID "@mozilla.org/cspservice;1"
#define CSPSERVICE_CID                               \
  {                                                  \
    0x8d2f40b2, 0x4875, 0x4c95, {                    \
      0x97, 0xd9, 0x3f, 0x7d, 0xca, 0x2c, 0xb4, 0x60 \
    }                                                \
  }
class CSPService : public nsIContentPolicy, public nsIChannelEventSink {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSICONTENTPOLICY
  NS_DECL_NSICHANNELEVENTSINK

  CSPService();

 protected:
  virtual ~CSPService();
};
#endif /* nsCSPService_h___ */
