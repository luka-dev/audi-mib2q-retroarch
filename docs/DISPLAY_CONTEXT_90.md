# Owning display context 90 from this repository

RetroArch needs a main-terminal display context whose layer list puts HMI and partial popups above
its video displayable — context `90`, layers `{16, 43}`.

That context was briefly declared in the CarPlay repository
(`luka-dev/mib2q-carplay-rgi-next`, `java_patch/de/audi/tghu/fwhmi/DisplayManagerMIB2High.java`) and
has been removed again. This document is why it cannot simply be patched in here the same way, and
what to do instead.

## Why it is not just another patched class

`DisplayManagerMIB2High` is a **stock** class. Both jars — this repository's `ra_mhi2q.jar` and
CarPlay's `carplay_hook.jar` — are deployed into the same directory,
`/mnt/app/eso/hmi/lsd/jars/`, and both are on j9's classpath. They coexist because they patch
*different* stock classes.

Put a patched `DisplayManagerMIB2High` in this jar and that stops being true: two jars in one
directory would carry two copies of one class, the classloader would pick one by an order nobody
controls, and the loser's changes would vanish silently. Nothing would fail at build time and
nothing would log an error — CarPlay's contexts 80/81 or RetroArch's 90 would simply not exist,
depending on which jar won that boot.

So the class stays owned by exactly one jar, and it is CarPlay's, because CarPlay patches it for its
own contexts already.

## What to do instead: extend the table at runtime

This repository already does exactly this kind of thing.
`com/luka/retroarch/inject/sm/RuntimeSmmInjector.java` locates a live OEM service, validates a table
fingerprint, builds complete replacement arrays and swaps the references in, without shadowing
anything in the boot path. The display-context table wants the same treatment.

The shape:

1. **Find the live instance.** `DisplayManagerMIB2High` is constructed with an `IFrameworkAccess`;
   reach it the same way the SMM injector reaches `SystemSMM`, rather than constructing one.
2. **Read the inherited `dc` field.** It is declared on the stock base class `DisplayManager`, not on
   `DisplayManagerMIB2High`, so walk up to the superclass when reflecting.
3. **Validate before writing.** The SMM injector's fingerprint check is the pattern: confirm the
   array length and that the entries you expect are the ones present, and refuse to touch it
   otherwise. An unexpected length means a different firmware or a different CarPlay build, and
   guessing there is how a display manager gets corrupted.
4. **Grow and copy.** Allocate `DisplayContext[91]`, `System.arraycopy` the old contents, and set
   index 90 to a `DisplayContext(90, new int[]{16, 43})`.
5. **Swap the reference**, then verify by reading it back.

Do it before the first `switchContext(90)`, not lazily inside it.

## The constraints that will bite

**Kombi type.** Declare 90 only when `framework.getKombiType() != KOMBI_TYPE_G24`. On G24 the stock
table is 158 entries and index 90 is already taken — it is stock's generated `11 + 79` KDK context,
where 79 is `G24_KDK_CTX_OFFSET`. Writing 90 there destroys a stock context. If RetroArch is ever
wanted on a G24 cluster it needs a different index, chosen from outside the `0..78` plus `+79`
ranges.

**CarPlay resizes the same array.** The CarPlay patch allocates `DisplayContext[82]` on A5 to hold
its own contexts 80 and 81. If you grow the array you must copy what is already in it, and if
CarPlay ever reallocates after you have swapped, your entry is gone. Ordering matters: extend after
CarPlay's display manager has finished its own initialisation, and re-check rather than assume.

**A stale index is silent.** `switchContext` on an index past the array end throws; on an index that
exists but holds `null` the failure is quieter. Verify the read-back rather than trusting the write.

## What the CarPlay side had, for reference

The removed declaration, so it does not have to be reconstructed:

```java
private static final int CTX_RETROARCH_MAIN = 90;   // RetroArch: HMI overlays over video
private static final int DC_SIZE_A5 = 91;           // stock 0..78 + CarPlay 80/81 + RetroArch 90

// in the non-G24 branch, alongside dc[80] and dc[81]:
this.dc[CTX_RETROARCH_MAIN] = new DisplayContext(CTX_RETROARCH_MAIN, new int[]{16, 43});
```

Layer `16` is the HMI/partial-popup plane and `43` is the video plane; the order is what keeps
popups drawn above RetroArch rather than behind it.

## If the runtime route turns out not to work

The fallback is not to patch the class here. It is to put the declaration back in the CarPlay
repository and record the dependency in both — RetroArch would then require a `carplay_hook.jar`
built with context 90, and neither repository's build would enforce that. That coupling is why it
was taken out; accept it only with the dependency written down on both sides.
