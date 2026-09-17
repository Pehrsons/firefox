/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://w3c.github.io/mediacapture-transform/#video-track-generator
 */

[Exposed=DedicatedWorker, Pref="media.mediastreamtrack.transform.enabled"]
interface VideoTrackGenerator {
  constructor();
  [Throws]
  readonly attribute WritableStream writable;
  attribute boolean muted;
  // [Throws] is not in the spec. It is here only until [[track]] exists, see
  // Bug 1991618.
  [Throws]
  readonly attribute MediaStreamTrack track;
};
