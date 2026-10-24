"""Derive a dog's play scene from its showcase scene (issue #1559).

    python tools/dog-authoring/make_play_scene.py            # all three breeds
    python tools/dog-authoring/make_play_scene.py --breed Samoyed

Reads `Assets/Scenes/<Scene>.olo` and the breed's `<Model>.rig.json` and writes
`Assets/Scenes/<Breed>Play.olo`. The play behaviour itself is breed-agnostic
(Scripts/LuaScripts/DogPlay.lua and PlayHand.lua); what differs per breed comes
from the rig the generator wrote (tools/dog-authoring/dog_clips.py):

  * MouthSocketBall / MouthSocketStick -- children of the dog on its "jaw", at
    the rig's `sockets.mouth` / `sockets.mouthStick` (jaw-local translation and
    rotation; a held prop's frame is the model frame at the hold pose).
  * PickUpReach -- where the ball lies when the jaw closes on it at PickUp's
    grab instant (`fetch.pickUpBall`, model space).
  * Pick-volume padding -- how deep the coat is, so a click on the fur hits.

Everything else is the same for every breed: the camera leaves the dog and
orbits a play spot, a hand that parts the coat, a ball and a stick, a navmesh
over the lawn, and the dog's navigation (root motion), head aim and script.
New entities get UUIDs derived from the breed and their name, so a re-run
writes the same file.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path

import yaml

ASSETS = Path(__file__).resolve().parents[2] / "OloEditor" / "SandboxProject" / "Assets"

BREEDS = {
    # breed: (showcase scene, model rig, play scene, coat depth for picking)
    "Golden": ("Dog", "Models/Dog/Dog.rig.json", "DogPlay", 0.035),
    "Bernese": ("Bernese", "Models/Bernese/Bernese.rig.json", "BernesePlay", 0.045),
    "Samoyed": ("Samoyed", "Models/Samoyed/Samoyed.rig.json", "SamoyedPlay", 0.06),
}

# Strands the play scene's coat is capped at (GroomComponent MaxRenderStrands);
# a breed not listed plays with its showcase coat whole.
PLAY_STRAND_BUDGET = {
    "Bernese": 1000000,
    "Samoyed": 1000000,
}

# Gameplay names regions, not bones; DogPlay's favourite spots are "Ears" and
# the underside of "Neck"/"Torso" (the chest).
REGIONS = "; ".join([
    "Head: head, jaw, brow_L, brow_R, lid_upper_L, lid_upper_R, lid_lower_L, lid_lower_R, tongue_01, tongue_02, tongue_03",
    "Ears: ear_L_01, ear_L_02, ear_L_03, ear_R_01, ear_R_02, ear_R_03",
    "Neck: neck_01, neck_02",
    "Torso: pelvis, spine_01, spine_02, spine_03, scapula_L, scapula_R",
    "Tail: tail_01, tail_02, tail_03, tail_04, tail_05, tail_06",
    "FrontLegs: upperarm_L, forearm_L, wrist_L, paw_front_L, upperarm_R, forearm_R, wrist_R, paw_front_R",
    "HindLegs: thigh_L, shin_L, hock_L, paw_rear_L, thigh_R, shin_R, hock_R, paw_rear_R",
])


def uuid_for(breed, name):
    digest = hashlib.sha256(f"olo-dog-play/{breed}/{name}".encode()).digest()
    return int.from_bytes(digest[:8], "little") & 0x7FFFFFFFFFFFFFFF


def euler_from_quat(q):
    """glm::eulerAngles of a glTF (x, y, z, w) quaternion: pitch, yaw, roll."""
    x, y, z, w = q
    pitch = math.atan2(2.0 * (y * z + w * x), w * w - x * x - y * y + z * z)
    yaw = math.asin(max(-1.0, min(1.0, -2.0 * (x * z - w * y))))
    roll = math.atan2(2.0 * (x * y + w * z), w * w + x * x - y * y - z * z)
    return [round(pitch, 7), round(yaw, 7), round(roll, 7)]


def transform(t=(0.0, 0.0, 0.0), r=(0.0, 0.0, 0.0), s=(1.0, 1.0, 1.0)):
    return {"Translation": [float(v) for v in t], "Rotation": [float(v) for v in r], "Scale": [float(v) for v in s]}


def entity(uuid, tag, parent=0, **components):
    e = {"Entity": uuid, "TagComponent": {"Tag": tag}}
    e.update(components)
    e["RelationshipComponent"] = {"ParentHandle": parent, "Children": []}
    return e


def body(mass, drag, angular_drag):
    return {"BodyType": 1, "Mass": mass, "LinearDrag": drag, "AngularDrag": angular_drag,
            "DisableGravity": False, "IsTrigger": False}


def build(breed):
    scene_name, rig_path, play_name, coat_depth = BREEDS[breed]
    scene = yaml.safe_load((ASSETS / "Scenes" / f"{scene_name}.olo").read_text(encoding="utf-8"))
    rig = json.loads((ASSETS / rig_path).read_text(encoding="utf-8"))
    entities = scene["Entities"]
    by_tag = {e["TagComponent"]["Tag"]: e for e in entities}
    dog, camera = by_tag["Dog"], by_tag["RuntimeCamera"]
    dog_id = dog["Entity"]
    scene["Scene"] = play_name
    # The dog walks up to the camera and away again, so its coat crosses LOD
    # steps all the time. Without representation streaming every crossing
    # rebuilt the strand stream (563 MiB at stride 2 on the golden) and baked
    # the coat volume again on the main thread: a one-second freeze. Streamed,
    # the step is prepared on workers while the floor (the card tier) draws.
    # Staging must hold the whole strand tier's preparation estimate and a
    # frame's upload the whole prepared tier, which is built in one frame, or
    # the tier is never started or never built and the coat stays on cards
    # however close it comes. The Samoyed's two-million-strand coat estimates
    # 8121 MiB to prepare and uploads 2061 MiB, the Bernese 6914 and 1784, the
    # golden 2140 and 568: at 1536 and 64 none of them ever left the cards.
    scene.setdefault("StreamingSettings", {}).update({
        "RepresentationResidentMegabytes": 4096,
        "RepresentationUploadMegabytesPerFrame": 2560,
        "RepresentationStagingMegabytes": 10240,
    })

    # The dense coats' strand budget while playing (#1559, #1558). At their full
    # two million (Samoyed) and 1.6 million (Bernese) strands the coat alone
    # took 18-21 ms of GPU on Vulkan and 25 ms on GL, and with presents missing
    # the display's refresh a playing dog's frames ran to 35-37 ms at p95. The
    # budget is the coat's own cap: the showcase scenes keep every strand.
    if breed in PLAY_STRAND_BUDGET:
        groom = by_tag["DogCoat"]["GroomComponent"]
        budget = PLAY_STRAND_BUDGET[breed]
        segments = groom.get("MaxRenderSegments")
        if segments:
            groom["MaxRenderSegments"] = int(round(segments * budget / groom["MaxRenderStrands"]))
        groom["MaxRenderStrands"] = budget

    # The camera leaves the dog: it orbits the play spot, and the dog runs off.
    camera["RelationshipComponent"]["ParentHandle"] = 0
    dog["RelationshipComponent"]["Children"] = [c for c in dog["RelationshipComponent"]["Children"] if c != camera["Entity"]]
    camera["TransformComponent"] = transform((1.45, 1.05, 2.25), (-0.33, 0.57, 0.0))
    camera["LuaScriptComponent"] = {"ScriptFile": "Scripts/LuaScripts/PlayHand.lua"}

    dog["PickVolumesComponent"] = {"Regions": REGIONS, "Padding": coat_depth, "RadiusScale": 1.0, "Enabled": True}
    dog["NavAgentComponent"] = {"Radius": 0.3, "Height": 0.8, "MaxSpeed": rig["runSpeed"], "Acceleration": 8.0,
                                "StoppingDistance": 0.15, "AvoidancePriority": 50, "LockYAxis": True,
                                "UseRootMotion": True, "TurnSpeed": 5.0}
    # The head aims (its local +Y faces out of the muzzle) and bends the neck
    # with it; DogPlay finds the head bone by name and drives the weight.
    dog["IKTargetComponent"] = {"AimIKEnabled": False, "AimBoneIndex": 0, "AimTarget": [0.0, 0.0, 0.0],
                                "AimAxis": [0.0, 1.0, 0.0], "AimOffset": [0.0, 0.0, 0.0], "AimPoleVector": [0.0, 1.0, 0.0],
                                "AimChainLength": 3, "AimChainFactor": 0.5, "AimWeight": 0.0, "AimTargetEntity": 0}
    dog["LuaScriptComponent"] = {"ScriptFile": "Scripts/LuaScripts/DogPlay.lua"}

    # A hand parts the coat only as finely as the coat's guides: the showcase
    # budgets (one guide every few centimetres) part it in blobs. Four times as
    # many guides of each role, and full simulation detail at play distance.
    coat = by_tag["DogCoat"]
    sim = coat["GroomSimulationComponent"]
    for key in ("MaxGuidesUnassigned", "MaxGuidesUndercoat", "MaxGuidesGuardHair", "MaxGuidesWhisker", "MaxGuidesLongHair"):
        sim[key] = int(sim[key]) * 4
    coat["GroomLodComponent"]["SimulationFullPixelSize"] = 96

    added = []
    for name, socket in (("MouthSocketBall", rig["sockets"]["mouth"]), ("MouthSocketStick", rig["sockets"]["mouthStick"])):
        uid = uuid_for(breed, name)
        added.append(entity(uid, name, dog_id,
                            TransformComponent=transform(socket["translation"], euler_from_quat(socket["rotation"])),
                            BoneAttachmentComponent={"BoneName": socket["bone"], "Enabled": True}))
        dog["RelationshipComponent"]["Children"].append(uid)
    reach = uuid_for(breed, "PickUpReach")
    added.append(entity(reach, "PickUpReach", dog_id, TransformComponent=transform(rig["fetch"]["pickUpBall"])))
    dog["RelationshipComponent"]["Children"].append(reach)

    added.append(entity(uuid_for(breed, "PlayFocus"), "PlayFocus", TransformComponent=transform((0.0, 0.38, 0.35))))
    added.append(entity(uuid_for(breed, "PlayArea"), "PlayArea", TransformComponent=transform(),
                        NavMeshBoundsComponent={"Min": [-20.0, -1.0, -20.0], "Max": [20.0, 3.0, 20.0], "Links": []}))
    # The hand: parked far below the lawn until it touches fur.
    added.append(entity(uuid_for(breed, "Hand"), "Hand", TransformComponent=transform((0.0, -50.0, 0.0)),
                        GroomColliderComponent={"Radius": 0.045, "HalfHeight": 0.0, "Enabled": False}))

    radius = rig["fetch"]["ballRadius"]
    ball, ball_mesh = uuid_for(breed, "ToyBall"), uuid_for(breed, "ToyBallMesh")
    ball_entity = entity(ball, "ToyBall", TransformComponent=transform((0.75, radius, 1.05)),
                         Rigidbody3DComponent=body(0.058, 0.25, 0.6),
                         SphereCollider3DComponent={"Radius": radius, "Offset": [0.0, 0.0, 0.0], "StaticFriction": 0.8,
                                                    "DynamicFriction": 0.7, "Restitution": 0.55},
                         # In the mouth, the ball pushes the muzzle's fur aside.
                         GroomColliderComponent={"Radius": radius, "HalfHeight": 0.0, "Enabled": True})
    ball_entity["RelationshipComponent"]["Children"] = [ball_mesh]
    added.append(ball_entity)
    added.append(entity(ball_mesh, "ToyBallMesh", ball, TransformComponent=transform(s=(2 * radius,) * 3),
                        MeshComponent={"Primitive": 2},
                        MaterialComponent={"AlbedoColor": [0.74, 0.86, 0.16], "Metallic": 0.0, "Roughness": 0.95}))

    # The stick lies along its local X (the rig's `longAxis`), so in the mouth it
    # is carried crosswise.
    half_length, thick = 0.2, rig["fetch"]["stickRadius"]
    stick, stick_mesh, stick_fur = (uuid_for(breed, n) for n in ("ToyStick", "ToyStickMesh", "ToyStickFur"))
    stick_entity = entity(stick, "ToyStick", TransformComponent=transform((-0.7, thick, 0.95), (0.0, 0.6, 0.0)),
                          Rigidbody3DComponent=body(0.15, 0.2, 0.8),
                          BoxCollider3DComponent={"HalfExtents": [half_length, thick, thick], "Offset": [0.0, 0.0, 0.0],
                                                  "StaticFriction": 0.9, "DynamicFriction": 0.8, "Restitution": 0.15})
    stick_entity["RelationshipComponent"]["Children"] = [stick_mesh, stick_fur]
    added.append(stick_entity)
    # The cylinder primitive stands on its Y: turned onto X.
    added.append(entity(stick_mesh, "ToyStickMesh", stick,
                        TransformComponent=transform(r=(0.0, 0.0, math.pi / 2), s=(2 * thick, 2 * half_length, 2 * thick)),
                        MeshComponent={"Primitive": 4},
                        MaterialComponent={"AlbedoColor": [0.36, 0.25, 0.15], "Metallic": 0.0, "Roughness": 0.85}))
    added.append(entity(stick_fur, "ToyStickFur", stick, TransformComponent=transform(r=(0.0, 0.0, math.pi / 2)),
                        GroomColliderComponent={"Radius": thick, "HalfHeight": half_length, "Enabled": True}))

    entities.extend(added)
    out = ASSETS / "Scenes" / f"{play_name}.olo"
    text = yaml.safe_dump(scene, sort_keys=False, default_flow_style=None, width=4096, allow_unicode=True)
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {out.relative_to(ASSETS.parent.parent.parent)} ({len(added)} play entities)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--breed", choices=sorted(BREEDS), action="append")
    args = parser.parse_args()
    for breed in args.breed or BREEDS:
        build(breed)


if __name__ == "__main__":
    main()
