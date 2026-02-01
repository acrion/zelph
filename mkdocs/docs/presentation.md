# zelph: A Video Introduction

This page provides an introduction to zelph through a video presentation held by Stefan Zipproth as part of his involvement with the [Wikidata Ontology Cleaning Task Force](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Cleaning_Task_Force). The talk covers the core concepts of zelph, its inference engine, and its application to Wikidata, including results from the Wikimedia Rapid Fund project. The video is published with the consent of all participants.

<video id="zelph-video" controls width="100%" preload="metadata">
  <source src="/assets/2026-01-13-zelph.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>

<script>
function jumpTo(time) {
  const video = document.getElementById('zelph-video');
  video.currentTime = time;
  video.play();  // Optional: Automatically start playing
}
</script>

## Video Sections

Click on a section to jump to the corresponding part of the video:

- <a href="#" onclick="jumpTo(0);">Introduction</a>: Overview of zelph and its purpose.
- <a href="#" onclick="jumpTo(99);">Basic Concept</a>: Fundamental ideas behind the semantic network.
- <a href="#" onclick="jumpTo(146);">Rules in Zelph</a>: How rules are defined and used.
- <a href="#" onclick="jumpTo(481);">The Inference Engine</a>: Details on the reasoning process.
- <a href="#" onclick="jumpTo(547);">Question: Quotation Marks?</a> Addressing common syntax questions.
- <a href="#" onclick="jumpTo(637);">No Edge Labels, but Nodes ⇒ Meta Reasoning About Predicates. In Wikidata terms: No difference between Q- und P-items</a>: Unique approach to relations.
- <a href="#" onclick="jumpTo(781);">Working with Wikidata</a>: Integration with Wikidata data.
- <a href="#" onclick="jumpTo(970);">The Pruned Wikidata Version</a>: Optimized versions for efficiency.
- <a href="#" onclick="jumpTo(1067);">The Synergy: zelph and Wikidata</a>: Benefits of combining both.
- <a href="#" onclick="jumpTo(1240);">Applying zelph scripts on Wikidata</a>: Practical script usage.
- <a href="#" onclick="jumpTo(1290);">Report of the Wikimedia Rapid Fund</a>: Outcomes and findings from the funded project.
- <a href="#" onclick="jumpTo(1562);">Running the Reasoning engine on Wikidata</a>: Demonstration of inference on large-scale data.

For more details on zelph, check out the [Quick Start Guide](quickstart.md) or the [Wikidata Integration](wikidata.md) page.