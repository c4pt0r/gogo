;;; gogo-mode.el --- Major mode for gogo source files (.gg) -*- lexical-binding: t; -*-

;;; Commentary:
;; gogo is Go syntax, so gogo-mode derives from go-mode (install go-mode
;; first) and adds highlighting for gogo's own //gogo: directives.
;;
;;   (add-to-list 'load-path "/path/to/gogo/editors/emacs")
;;   (require 'gogo-mode)

;;; Code:

(require 'go-mode)

(defconst gogo-mode--directive-keywords
  '(("^[ \t]*\\(//gogo:[A-Za-z]+\\)" 1 font-lock-preprocessor-face t))
  "Font-lock keywords for gogo's //gogo: directives.")

;;;###autoload
(define-derived-mode gogo-mode go-mode "gogo"
  "Major mode for gogo source files: go-mode plus //gogo: directives."
  (font-lock-add-keywords nil gogo-mode--directive-keywords))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.gg\\'" . gogo-mode))
;;;###autoload
(add-to-list 'interpreter-mode-alist '("gogo" . gogo-mode))

(provide 'gogo-mode)
;;; gogo-mode.el ends here
