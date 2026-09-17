/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://w3c.github.io/mediacapture-transform/#track-processor
 */

[Exposed=DedicatedWorker, Pref="media.mediastreamtrack.transform.enabled"]
interface MediaStreamTrackProcessor {
  [Throws]
  constructor(MediaStreamTrackProcessorInit init);
  readonly attribute ReadableStream readable;
  readonly attribute unsigned long long discardedFrames;
  readonly attribute unsigned long long totalFrames;
};

dictionary MediaStreamTrackProcessorInit {
  required MediaStreamTrack track;
  [EnforceRange] unsigned short maxBufferSize;
};
