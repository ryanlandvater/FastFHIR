# FastFHIR Generator — model subpackage.
# FHIR schema -> internal layout model. Pure Python data + transforms.
# This subpackage MUST NOT import from `emit/` or `bindings/`; the dependency
# direction is one-way (model <- emit), enforced so a cycle becomes an
# import error. See generator_refactor_plan.md section 2.2.
