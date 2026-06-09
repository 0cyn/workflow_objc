This directory contains two separate test paths. 

* Standard unit/regression tests in TestHelpers.cpp
* Corpus tests that generally just run and dump HLIL output from tests.

The corpus tests can be used to analyze effects of changes on the workflow. These will likely fail to provide consistent output
due to general BN nondeterminism but can be useful for getting a broad idea concerning side effects
of changes. 
