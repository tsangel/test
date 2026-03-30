# DICOM Dictionary Update Diff

- Version: 2026a -> 2026b
- Scope: TSV outputs only

## Key TSV Change Status

- _uid_registry.tsv: no changes
- _specific_character_sets.tsv: no changes

## Diffstat

```diff
 misc/dictionary/_dataelement_registry.tsv | 3 +++
 1 file changed, 3 insertions(+)
```

## File Diffs

### misc/dictionary/_dataelement_registry.tsv

```diff
diff --git a/misc/dictionary/_dataelement_registry.tsv b/misc/dictionary/_dataelement_registry.tsv
index f150a92..e3bb55e 100644
--- a/misc/dictionary/_dataelement_registry.tsv
+++ b/misc/dictionary/_dataelement_registry.tsv
@@ -1462,6 +1462,9 @@ tag	name	keyword	vr	vm	retired
 (0018,9382)	Material Attenuation Sequence	MaterialAttenuationSequence	SQ	1	
 (0018,9383)	Photon Energy	PhotonEnergy	DS	1	
 (0018,9384)	X-Ray Mass Attenuation Coefficient	XRayMassAttenuationCoefficient	DS	1	
+(0018,9390)	Metal Artifact Reduction Sequence	MetalArtifactReductionSequence	SQ	1	
+(0018,9391)	Metal Artifact Reduction Applied	MetalArtifactReductionApplied	CS	1	
+(0018,9392)	Metal Artifact Reduction Algorithm Identification Sequence	MetalArtifactReductionAlgorithmIdentificationSequence	SQ	1	
 (0018,9401)	Projection Pixel Calibration Sequence	ProjectionPixelCalibrationSequence	SQ	1	
 (0018,9402)	Distance Source to Isocenter	DistanceSourceToIsocenter	FL	1	
 (0018,9403)	Distance Object to Table Top	DistanceObjectToTableTop	FL	1	
```

