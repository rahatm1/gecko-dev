/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ConvolverNode_h_
#define ConvolverNode_h_

#include "AudioNode.h"
#include "AudioBuffer.h"

namespace mozilla {
namespace dom {

class ConvolverNode : public AudioNode
{
public:
  explicit ConvolverNode(AudioContext* aContext);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(ConvolverNode, AudioNode);

  virtual JSObject* WrapObject(JSContext* aCx) MOZ_OVERRIDE;

  AudioBuffer* GetBuffer(JSContext* aCx) const
  {
    return mBuffer;
  }

  void SetBuffer(JSContext* aCx, AudioBuffer* aBufferi, ErrorResult& aRv);

  bool Normalize() const
  {
    return mNormalize;
  }

  void SetNormalize(bool aNormal);

  virtual void SetChannelCount(uint32_t aChannelCount, ErrorResult& aRv) MOZ_OVERRIDE
  {
    if (aChannelCount > 2) {
      aRv.Throw(NS_ERROR_DOM_NOT_SUPPORTED_ERR);
      return;
    }
    AudioNode::SetChannelCount(aChannelCount, aRv);
  }
  virtual void SetChannelCountModeValue(ChannelCountMode aMode, ErrorResult& aRv) MOZ_OVERRIDE
  {
    if (aMode == ChannelCountMode::Max) {
      aRv.Throw(NS_ERROR_DOM_NOT_SUPPORTED_ERR);
      return;
    }
    AudioNode::SetChannelCountModeValue(aMode, aRv);
  }

  virtual const char* NodeType() const
  {
    return "ConvolverNode";
  }

  virtual size_t SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const MOZ_OVERRIDE;
  virtual size_t SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const MOZ_OVERRIDE;

private:
  nsRefPtr<AudioBuffer> mBuffer;
  bool mNormalize;
};


} //end namespace dom
} //end namespace mozilla

#endif

