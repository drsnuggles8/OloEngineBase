# What is drawn without a depth test is drawn in an order the scene defines

Applies to every loop in `Scene` that submits blended or depth-untested primitives: world text,
sprites, circles, and anything added to the UI composite callback or the 2D paths.

## Rules

1. **Never draw such primitives in the order a registry view yields them.** That order is not a
   property of the scene: `Scene::Copy` yields the same entities in another order.
2. **Draw them farthest from the camera first, ties by UUID.** `Scene::CollectBackToFront<T>` does
   it; `DrawWorldSprites`, `DrawWorldCircles` and `DrawWorldText` are the three callers. The
   nearest one is drawn last and lies on top. UUID is the tie-break because a copy keeps it.
3. **Test the order on a copy of the scene, not on the scene twice.** Edit, Simulate and Play draw
   through the same code; what differs between them is that Simulate and Play draw a copy.
   `WorldTextOrderTest` compares a scene with `Scene::Copy` of it.

## What happened (#1700)

In the editor, two overlapping `TextComponent`s in `FontRenderingTest.olo` swapped which one was on
top when Simulate started: 15 pixels in a 7 x 3 box, the two texts' own colours exchanged. Edit
against Edit after Stop was identical, and so were two Simulate frames.

World text is drawn by the UI composite pass with the depth test off, in the order
`m_Registry.view<TransformComponent, TextComponent>()` yields. The registry of the copy Simulate
runs on yields the reverse. Sprites and circles go through the same loops: in a copy, two
overlapping sprites showed 2,016 red pixels where the original showed 2,433.

It had not been seen because overlapping text is rare and a 2D scene is usually looked at in one
mode at a time.
