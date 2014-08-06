--TEST--
Put - PUT With Second Parameter not an array.

--SKIPIF--
<?php
include dirname(__FILE__)."/../../astestframework/astest-phpt-loader.inc";
aerospike_phpt_skipif("Put", "testCheckSecondParameterTypeArray");

--FILE--
<?php
include dirname(__FILE__)."/../../astestframework/astest-phpt-loader.inc";
aerospike_phpt_runtest("Put", "testCheckSecondParameterTypeArray");
--EXPECT--
Parameter_Exception
