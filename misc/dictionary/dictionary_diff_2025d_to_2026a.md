# DICOM Dictionary Update Diff

- Version: 2025d -> 2026a
- Scope: TSV outputs only

## Key TSV Change Status

- _uid_registry.tsv: no changes
- _specific_character_sets.tsv: no changes

## Diffstat

```diff
 misc/dictionary/_dataelement_registry.tsv | 8 +++++++-
 1 file changed, 7 insertions(+), 1 deletion(-)
```

## File Diffs

### misc/dictionary/_dataelement_registry.tsv

```diff
diff --git a/misc/dictionary/_dataelement_registry.tsv b/misc/dictionary/_dataelement_registry.tsv
index 9a8feca..f150a92 100644
--- a/misc/dictionary/_dataelement_registry.tsv
+++ b/misc/dictionary/_dataelement_registry.tsv
@@ -15,6 +15,7 @@ tag	name	keyword	vr	vm	retired
 (0008,001A)	Related General SOP Class UID	RelatedGeneralSOPClassUID	UI	1-n	
 (0008,001B)	Original Specialized SOP Class UID	OriginalSpecializedSOPClassUID	UI	1	
 (0008,001C)	Synthetic Data	SyntheticData	CS	1	
+(0008,001D)	Sensitive Content Code Sequence	SensitiveContentCodeSequence	SQ	1	
 (0008,0020)	Study Date	StudyDate	DA	1	
 (0008,0021)	Series Date	SeriesDate	DA	1	
 (0008,0022)	Acquisition Date	AcquisitionDate	DA	1	
@@ -686,7 +687,7 @@ tag	name	keyword	vr	vm	retired
 (0014,604E)	Moving Window Weights	MovingWindowWeights	DS	1-n	DICONDE
 (0014,604F)	Moving Window Pitch	MovingWindowPitch	DS	1	DICONDE
 (0014,6050)	Moving Window Padding Scheme	MovingWindowPaddingScheme	CS	1	DICONDE
-(0014,6051)	Moving Window Padding Sength	MovingWindowPaddingLength	DS	1	DICONDE
+(0014,6051)	Moving Window Padding Length	MovingWindowPaddingLength	DS	1	DICONDE
 (0014,6052)	Spatial Filtering Parameters Sequence	SpatialFilteringParametersSequence	SQ	1	DICONDE
 (0014,6053)	Spatial Filtering Scheme	SpatialFilteringScheme	CS	1	DICONDE
 (0014,6056)	Horizontal Moving Window Size	HorizontalMovingWindowSize	DS	1	DICONDE
@@ -4042,6 +4043,11 @@ tag	name	keyword	vr	vm	retired
 (3004,0012)	Dose Value	DoseValue	DS	1	
 (3004,0014)	Tissue Heterogeneity Correction	TissueHeterogeneityCorrection	CS	1-3	
 (3004,0016)	Recommended Isodose Level Sequence	RecommendedIsodoseLevelSequence	SQ	1	
+(3004,0020)	Dose Unit Code Sequence	DoseUnitCodeSequence	SQ	1	
+(3004,0021)	RT Dose Interpreted Type Code Sequence	RTDoseInterpretedTypeCodeSequence	SQ	1	
+(3004,0022)	RT Dose Interpreted Type Code Modifier Sequence	RTDoseInterpretedTypeCodeModifierSequence	SQ	1	
+(3004,0023)	Dose Radiobiological Interpretation Sequence	DoseRadiobiologicalInterpretationSequence	SQ	1	
+(3004,0024)	RT Dose Intent Code Sequence	RTDoseIntentCodeSequence	SQ	1	
 (3004,0040)	DVH Normalization Point	DVHNormalizationPoint	DS	3	
 (3004,0042)	DVH Normalization Dose Value	DVHNormalizationDoseValue	DS	1	
 (3004,0050)	DVH Sequence	DVHSequence	SQ	1	
```

