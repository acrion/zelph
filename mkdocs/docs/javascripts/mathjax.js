// MathJax configuration for the pymdownx.arithmatex "generic" output.
//
// Delimiters are \( ... \) and \[ ... \] only -- no dollars. "$(" is
// zelph's term-island syntax and occurs in prose all over the mathematics
// section, so dollar-delimited math would collide with it (mkdocs.yml
// restricts arithmatex to the round/square syntaxes for the same reason).
//
// processHtmlClass is a second, independent guard: MathJax only looks
// inside the spans arithmatex emits, so nothing else on the page can be
// mistaken for math.
window.MathJax = {
  tex: {
    inlineMath: [["\\(", "\\)"]],
    displayMath: [["\\[", "\\]"]],
    processEscapes: true,
    processEnvironments: true
  },
  options: {
    ignoreHtmlClass: ".*|",
    processHtmlClass: "arithmatex"
  }
};

// Re-typeset after Material's instant navigation swaps the page body.
// Without this, formulas render on a full page load but not when the
// reader arrives via an internal link.
if (typeof document$ !== "undefined") {
  document$.subscribe(function () {
    if (window.MathJax && window.MathJax.typesetPromise) {
      window.MathJax.typesetPromise();
    }
  });
}
