/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://w3c.github.io/mediacapture-main/#mediastreamtrackevent
 */

dictionary MediaStreamTrackEventInit : EventInit {
    required MediaStreamTrack track;
};

// Exposure in DedicatedWorker is gated on the pref
// media.mediastreamtrack.transferable.enabled through IsExposed, see
// https://w3c.github.io/mediacapture-extensions/#mediastream-in-dedicated-workers
[Exposed=(Window,DedicatedWorker),
 Func="mozilla::dom::MediaStreamTrack::IsExposed"]
interface MediaStreamTrackEvent : Event {
    constructor(DOMString type, MediaStreamTrackEventInit eventInitDict);

    [SameObject]
    readonly        attribute MediaStreamTrack track;
};
