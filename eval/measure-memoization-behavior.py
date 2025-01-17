#!/usr/bin/env python3
# Test a libLF.SimpleRegex for SL behavior under various memoization schemes.
# This analysis includes time measurements for performance.
# Run it alone, or use core pinning (e.g. taskset) to reduce interference

# Import libMemo
import libMemo

# Import libLF
import os
import sys
sys.path.append(os.path.join(os.environ['MEMOIZATION_PROJECT_ROOT'], 'eval', 'lib'))
import libLF

import re
import json
import tempfile
import argparse
import traceback
import time
import subprocess
import statistics
import pandas as pd

# Shell dependencies

PRODUCTION_ENGINE_CLI_ROOT = os.path.join(os.environ['MEMOIZATION_PROJECT_ROOT'], 'eval', 'query-production-engines')

PRODUCTION_ENGINE_TO_CLI = {
  "perl":   os.path.join(PRODUCTION_ENGINE_CLI_ROOT, 'perl', 'query-perl.pl'),
  "php":    os.path.join(PRODUCTION_ENGINE_CLI_ROOT, 'php', 'query-php.php'),
  "csharp": os.path.join(PRODUCTION_ENGINE_CLI_ROOT, 'csharp', 'QueryCSharp.exe'),
}

shellDeps = [ libMemo.ProtoRegexEngine.CLI, *PRODUCTION_ENGINE_TO_CLI.values() ]

# Other globals
PROTOTYPE_SL_MATCH_TIMEOUT = 2 # Seconds before timing out SL queries to our prototype
GROWTH_RATE_INF = "INF"

PROTOTYPE_MEMO_MATCH_TIMEOUT = 180 # Seconds before timing out memoized queries to our prototype

CHECK_TIME_COEFFICIENT_OF_VARIANCE = False

SAVE_TMP_FILES = False

EXPAND_EVIL_INPUT = True # Get more SL regexes, corrects some common errors

##########

class EngineBehavior:
  """Characterize behavior of production regex engines"""
  InvalidRegex = "InvalidRegex"

  MatchCompleted = "MatchCompleted" # (Presumably in linear time, e.g. because of optimizations)
  RuntimeException = "Runtime exception" # e.g. resource measurement
  TimeoutException = "Timeout exception" # C#

  SuperLinear = "Super-linear behavior" # We terminated the match ourselves

class TaskConfig:
  """Describes which tasks we should perform"""
  RunSecurityAnalysis = "Security analysis"
  QueryPrototype = "Query prototype"
  QueryProductionEngines = "Query production engines"

  def __init__(self, useCSharpToFindMostEI, queryPrototype, runSecurityAnalysis, queryProductionEngines):
    self.tasks = []
    self.useCSharpToFindMostEI = useCSharpToFindMostEI

    if runSecurityAnalysis:
      self.tasks.append(TaskConfig.RunSecurityAnalysis)
      return

    if queryPrototype:
      self.tasks.append(TaskConfig.QueryPrototype)
    
    if queryProductionEngines:
      self.tasks.append(TaskConfig.QueryProductionEngines)

  def runSecurityAnalysis(self):
    return TaskConfig.RunSecurityAnalysis in self.tasks
  
  def queryPrototype(self):
    return TaskConfig.QueryPrototype in self.tasks

  def queryProductionEngines(self):
    return TaskConfig.QueryProductionEngines in self.tasks

class MyTask(libLF.parallel.ParallelTask): # Not actually parallel, but keep the API
  NOT_SL = "NOT_SL"

  # Take 5 samples per evil input
  # Need at least 3 to compute second derivative
  # But keep values low to avoid too-long backtracking stacks
  MOSTEI_PUMPS = [ i*3 for i in range(1,5) ]

  # Can be much longer because memoization prevents geometric growth of the backtracking stack
  # perfPumps = [ 1000 ] # i*500 for i in range(1,5) ]

  #PROD_ENGINE_PUMPS = 500 * 1000
  #PROD_ENGINE_PUMPS = 200 * 1000 # Takes 27 seconds in Node.js on my MacBook. 100K takes 7 seconds, but wine is slow to start (sometimes 5 seconds) so we want to play it safe with a 10 second timeout.
  #PROD_ENGINE_PUMPS = 1 * 1000
  #PROD_ENGINE_PUMPS = 900

  # perfPumps = [ int(PROD_ENGINE_PUMPS/10) ] # Hmm?

  SECURITY_ANALYSIS_PUMPS = list(range(int(1e4), int(1e5), int(1e4))) # 10K, 20K, ..., 100K
  SECURITY_ANALYSIS_SELECTION = libMemo.ProtoRegexEngine.SELECTION_SCHEME.allMemo
  SECURITY_ANALYSIS_ENCODING = libMemo.ProtoRegexEngine.ENCODING_SCHEME.ES_None

  def __init__(self, regex, perfPumps, queryProtoMaxAttackStringLen, nTrialsPerCondition, taskConfig):
    self.regex = regex
    self.perfPumps = [ perfPumps ]
    self.queryProtoMaxAttackStringLen = queryProtoMaxAttackStringLen
    self.nTrialsPerCondition = nTrialsPerCondition
    self.taskConfig = taskConfig
  
  def run(self):
    """Run task
    
    Returns:
      If SL: a pd.DataFrame with the data when run under perfPumps[-1]
      Else: MyTask.NOT_SL
      Captures and returns non-KeyboardInterrupt exceptions raised during execution
    Raises: KeyboardInterrupt
    """
    try:
      libLF.log('Working on regex: /{}/'.format(self.regex.pattern))

      # Filter out non-SL regexes
      libLF.log("  TASK: Confirming that regex is SL")
      if self.taskConfig.useCSharpToFindMostEI:
        queriedWith = "C#"
        ei = self._findAnySLInputUsingCSharp(self.regex)
      else:
        queriedWith = "prototype"
        ei, _ = self._findMostSLInput(self.regex)

      if ei is None:
        libLF.log("  Could not trigger SL behavior using {}".format(queriedWith))
        return MyTask.NOT_SL
      else:
        libLF.log("  Triggered SL behavior using {}".format(queriedWith))
      
      if self.taskConfig.runSecurityAnalysis():
        libLF.log("  TASK: Running security analysis")
        # This violates the function API, but it's late and I'm tired
        return self._runSecurityAnalysis(self.regex, ei)

      if self.taskConfig.queryPrototype():
        libLF.log("  TASK: Running analysis on SL regex")
        self.pump_to_mdas = self._runSLDynamicAnalysis(self.regex, ei, self.nTrialsPerCondition)
      else:
        fakeMDA = libMemo.MemoizationDynamicAnalysis()
        fakeMDA.initFromRaw(self.regex.pattern, -1, -1, -1, -1, ei, self.perfPumps[-1], {
          libMemo.ProtoRegexEngine.SELECTION_SCHEME.SS_Full: { # Algo
            libMemo.ProtoRegexEngine.ENCODING_SCHEME.ES_None: -1,
          },
          libMemo.ProtoRegexEngine.SELECTION_SCHEME.SS_Full: { # Space
            libMemo.ProtoRegexEngine.ENCODING_SCHEME.ES_None: -1,
          },
        }, {
          libMemo.ProtoRegexEngine.SELECTION_SCHEME.SS_Full: { # Time
            libMemo.ProtoRegexEngine.ENCODING_SCHEME.ES_None: -1,
          },
        })
        self.pump_to_mdas = {
          self.perfPumps[-1]: fakeMDA
        }

      if self.taskConfig.queryProductionEngines():
        libLF.log("  TASK: Querying production regex engines")
        productionEngine_to_behavior = self._measureProductionEngineBehavior(self.regex, ei)
        libLF.log("Findings: {}".format(productionEngine_to_behavior))

      # Return
      libLF.log('Completed regex /{}/'.format(self.regex.pattern))
      # Just return the biggest one for now
      lastMDA = self.pump_to_mdas[self.perfPumps[-1]]

      # Collect pump data for the engines we tested
      if self.taskConfig.queryProductionEngines():
        lastMDA.productionEnginePumps = MyTask.PROD_ENGINE_PUMPS
        if 'perl' in PRODUCTION_ENGINE_TO_CLI:
          libLF.log("perl behavior: {}".format(productionEngine_to_behavior["perl"]))
          lastMDA.perlBehavior = productionEngine_to_behavior["perl"]
        if 'php' in PRODUCTION_ENGINE_TO_CLI:
          libLF.log("php behavior: {}".format(productionEngine_to_behavior["php"]))
          lastMDA.phpBehavior = productionEngine_to_behavior["php"]
        if 'csharp' in PRODUCTION_ENGINE_TO_CLI:
          libLF.log("csharp behavior: {}".format(productionEngine_to_behavior["csharp"]))
          lastMDA.csharpBehavior = productionEngine_to_behavior["csharp"]
      return lastMDA.toDataFrame()
    except KeyboardInterrupt:
      raise
    except BaseException as err:
      libLF.log('Exception while testing regex /{}/: {}'.format(self.regex.pattern, err))
      traceback.print_exc()
      return err
  
  def _measureProductionEngineBehavior(self, regex, evilInput, maxQuerySec=10, useCSharpTimeout=True, engines=PRODUCTION_ENGINE_TO_CLI.keys()):
    """Returns { "perl": EngineBehavior, "php": eb, "csharp": eb }"""

    if useCSharpTimeout:
      csharpTimeoutMS = 10
    else:
      csharpTimeoutMS = -1

    engine_to_behavior = {}
    for engine in engines:
      engine_to_behavior[engine] = self._queryProductionEngine(regex, evilInput, maxQuerySec, engine, csharpTimeoutMS)

    return engine_to_behavior
  
  def _engineOutputToBehavior(self, engineWrapperStdout):
    INVALID_INPUT = "INVALID_INPUT"
    PHP_EXC_SNIPPET = "PREG"
    PERL_EXC_SNIPPET = "RECURSION_LIMIT"
    CSHARP_TIMEOUT_SNIPPET = "timed out"

    obj = json.loads(engineWrapperStdout)
    libLF.log("exceptionString: {}".format(obj['exceptionString']))
    if obj['exceptionString'] == INVALID_INPUT:
      return EngineBehavior.InvalidRegex
    elif PHP_EXC_SNIPPET in obj['exceptionString'] \
      or PERL_EXC_SNIPPET in obj['exceptionString']:
      return EngineBehavior.RuntimeException
    elif CSHARP_TIMEOUT_SNIPPET in obj['exceptionString']:
      return EngineBehavior.TimeoutException
    
    return EngineBehavior.MatchCompleted
  
  def _buildQueryFileForProductionRegexEngine(self, regex, mostEI, nPumps, timeoutMS=-1):
    """Return the path to a query file
    
    Unlike libMemo.ProtoRegexEngine.buildQueryFile, the production regex engine query tools
       take evilInput and nPumps instead of the raw string
    """
    fd, name = tempfile.mkstemp(suffix=".json", prefix="measure-memoization-behavior-queryFile-")
    os.close(fd)

    evilInput = { "pumpPairs": [], "suffix": mostEI.suffix }
    for pp in mostEI.pumpPairs:
      evilInput["pumpPairs"].append({
        "prefix": pp.prefix,
        "pump": pp.pump
      })

    with open(name, 'w') as outStream:
        json.dump({
            "pattern": regex.pattern,
            "evilInput": evilInput,
            "nPumps": nPumps,
            "timeoutMS": timeoutMS
        }, outStream)
    return name

  def _queryProductionEngine(self, regex, mostEI, maxQuerySec, engine, timeoutMS):
    """Returns EngineBehavior for this engine"""
    # Query file
    queryFile = self._buildQueryFileForProductionRegexEngine(regex, mostEI, MyTask.PROD_ENGINE_PUMPS, timeoutMS=timeoutMS)

    if engine == "csharp":
      args = [ "wine", PRODUCTION_ENGINE_TO_CLI[engine], queryFile ]
    else:
      args = [ PRODUCTION_ENGINE_TO_CLI[engine], queryFile ]
    libLF.log("Querying {}: {}".format(engine, " ".join(args)))
    child = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0, close_fds=1)
    wrapperTimedOut = False
    try:
      stdout, stderr = child.communicate(timeout=maxQuerySec)
      #libLF.log("stdout: {}".format(stdout))
      #libLF.log("stderr: {}".format(stderr))
    except subprocess.TimeoutExpired:
      wrapperTimedOut = True

    # Paranoid cleanup
    try:
      child.terminate()
      child.kill()
    except:
      pass
    
    # Normal cleanup
    if not SAVE_TMP_FILES:
      os.unlink(queryFile)

    # Convert to EngineBehavior
    if wrapperTimedOut:
      return EngineBehavior.SuperLinear
    else:
      return self._engineOutputToBehavior(stdout.decode('utf-8'))
  
  def _measureCondition(self, regex, mostEI, nPumps, maxAttackStringLen, selectionScheme, encodingScheme, nTrialsPerCondition):
    """Obtain the average time and space costs for this condition
    
    Returns: automatonSize (integer), phiSize (integer), time (numeric), algoSpace (numeric), bytesSpace (numeric)
    """
    queryFile = libMemo.ProtoRegexEngine.buildQueryFile(regex.pattern, mostEI.build(nPumps, maxAttackStringLen)[0], rleKValue=regex.rleKValue)

    measures = []
    for i in range(0, nTrialsPerCondition):
      try:
        meas = libMemo.ProtoRegexEngine.query(selectionScheme, encodingScheme, queryFile, timeout=PROTOTYPE_MEMO_MATCH_TIMEOUT)
        measures.append(meas)
      except BaseException as err:
        libLF.log(err)
        libLF.log("Error, a timeout should not have occurred with memoization in place")
        raise

    if not SAVE_TMP_FILES:
      os.unlink(queryFile)

    indivTimeCosts = [ meas.si_simTimeUS for meas in measures ]
    indivSpaceCostsAlgo = [
      sum(meas.mi_results_maxObservedAsymptoticCostsPerVertex)
      for meas in measures
    ]
    indivSpaceCostsBytes = [
      sum(meas.mi_results_maxObservedMemoryBytesPerVertex)
      for meas in measures
    ]

    # Space costs should be constant
    assert min(indivSpaceCostsAlgo) == max(indivSpaceCostsAlgo), "Space costs are not constant"
    assert min(indivSpaceCostsBytes) == max(indivSpaceCostsBytes), "Space costs are not constant"

    if CHECK_TIME_COEFFICIENT_OF_VARIANCE:
      # Let's check that time costs do not vary too much, warn if it's too high
      time_coefficientOfVariance = statistics.stdev(indivTimeCosts) / statistics.mean(indivTimeCosts)
      libLF.log("Time coefficient of variance: {}".format(round(time_coefficientOfVariance, 2)))
      if 0.5 < time_coefficientOfVariance:
        libLF.log("Warning, time CV {} was >= 0.5".format(time_coefficientOfVariance))

    # Condense
    automatonSize = measures[0].ii_nStates
    phiSize = measures[0].mi_results_nSelectedVertices
    time = statistics.median_low(indivTimeCosts)
    spaceAlgo = indivSpaceCostsAlgo[0] # Constant 
    spaceBytes = indivSpaceCostsBytes[0] # Constant
    
    return automatonSize, phiSize, time, spaceAlgo, spaceBytes
  
  def _runSecurityAnalysis(self, regex, ei):
    # The paper says we check that the number of visits |w| grows linearly with input size
    # Under the Full-None configuration, the prototype tests an assert that #visits <= |Q|x|w|
    # If the program runs without error, then the test passes.
    for selMode in MyTask.SECURITY_ANALYSIS_SELECTION:
      nVisits = []
      for nPumps in MyTask.SECURITY_ANALYSIS_PUMPS:
        libLF.log("Trying with {} pumps".format(nPumps))
        queryFile = libMemo.ProtoRegexEngine.buildQueryFile(regex.pattern, ei.build(nPumps)[0])
        try:
          em = libMemo.ProtoRegexEngine.query(
            selMode, MyTask.SECURITY_ANALYSIS_ENCODING, queryFile
          )
          nVisits.append(em.si_nTotalVisits)
        except BaseException as err: # Raises on rc != 0
          libLF.log("Exception on {} pumps: {}".format(nPumps, str(err)))
          return False

      # If we got this far, confirm that growth is linear
      diffs = [ next - prev for prev, next in zip(nVisits, nVisits[1:]) ]
      if len(set(diffs)) != 1:
        libLF.log("{}: Non-linear growth for /{}/. Diffs: {}".format(selMode, regex.pattern, diffs))
        return False

      libLF.log("{}: Linear growth for /{}/ (matches theorem)".format(selMode, regex.pattern))

    # No error on any seletcion scheme
    return True

  def _runSLDynamicAnalysis(self, regex, mostEI, nTrialsPerCondition):
    """Obtain MDAs for this <regex, EI> pair
    
    returns: pump_to_libMemo.MemoizationDynamicAnalysis
    """

    selectionSchemes = libMemo.ProtoRegexEngine.SELECTION_SCHEME.allMemo
    encodingSchemes = libMemo.ProtoRegexEngine.ENCODING_SCHEME.all

    selectionScheme_to_encodingScheme2engineMeasurement = {}

    # How many experimental conditions?
    nConditions = len(selectionSchemes) * len(encodingSchemes)
    libLF.log("    {} experimental conditions".format(nConditions))

    # Obtain engine measurements for each combination of the
    # memoization selection and encoding schemes
    pump_to_mda = {}
    for nPumps in self.perfPumps:

      # Prep an MDA
      mda = libMemo.MemoizationDynamicAnalysis()
      mda.pattern = regex.pattern
      mda.rleKValue = regex.rleKValue
      mda.inputLength = len(mostEI.build(nPumps)[0])
      mda.evilInput = mostEI
      mda.nPumps = nPumps

      conditionIx = 1
      for selectionScheme in selectionSchemes:
        selectionScheme_to_encodingScheme2engineMeasurement[selectionScheme] = {}
        for encodingScheme in encodingSchemes:
          libLF.log("    Trying selection/encoding combination {}/{}".format(conditionIx, nConditions))
          conditionIx += 1

          automatonSize, phiSize, timeCost, spaceCostAlgo, spaceCostBytes = self._measureCondition(regex, mostEI, nPumps, self.queryProtoMaxAttackStringLen, selectionScheme, encodingScheme, nTrialsPerCondition)
          libLF.log("{}: space cost algo {}, space cost bytes {}".format(encodingScheme, spaceCostAlgo, spaceCostBytes))

          # Automaton statistics
          mda.automatonSize = automatonSize
          if selectionScheme == libMemo.ProtoRegexEngine.SELECTION_SCHEME.SS_InDeg:
            mda.phiInDeg = phiSize
          elif selectionScheme == libMemo.ProtoRegexEngine.SELECTION_SCHEME.SS_Loop:
            mda.phiQuantifier = phiSize

          mda.selectionPolicy_to_enc2time[selectionScheme][encodingScheme] = timeCost
          mda.selectionPolicy_to_enc2spaceAlgo[selectionScheme][encodingScheme] = spaceCostAlgo
          mda.selectionPolicy_to_enc2spaceBytes[selectionScheme][encodingScheme] = spaceCostBytes

      # Did we screw up?
      mda.validate()

      pump_to_mda[nPumps] = mda
    return pump_to_mda
  
  def _findAnySLInputUsingCSharp(self, regex):
    libLF.log('Testing whether this regex exhibits SL behavior in C#: /{}/'.format(regex.pattern))
    assert regex.evilInputs, "regex has no evil inputs"

    if EXPAND_EVIL_INPUT:
      # Expand the evil inputs.
      # The longer expansions may have higher growth rates (larger polynomials),
      #   but may be buggy and not trigger mismatches properly.
      expandedEIs = []
      for ei in regex.evilInputs:
        expandedEIs += ei.expand()
      libLF.log("Considering {} EvilInput's, expanded from {}".format(len(expandedEIs), len(regex.evilInputs)))
      evilInputs = expandedEIs
    else:
      evilInputs = regex.evilInputs

    eng = 'csharp'
    for ei in regex.evilInputs:
      engineBehavior = self._measureProductionEngineBehavior(regex, ei, maxQuerySec=10, useCSharpTimeout=False, engines=[eng])
      if engineBehavior[eng] == EngineBehavior.SuperLinear:
        return ei
    return None

  def _findMostSLInput(self, regex):
    """Of a regex's evil inputs, identify the one that yields the MOST SL behavior.

    returns: libLF.EvilInput, itsGrowthRate
             None, -1 if all are linear-time (e.g. PROTOTYPE's semantics differ from PCRE)
    """
    libLF.log('Testing whether this regex exhibits SL behavior: /{}/'.format(regex.pattern))
    assert regex.evilInputs, "regex has no evil inputs"

    if EXPAND_EVIL_INPUT:
      # Expand the evil inputs.
      # The longer expansions may have higher growth rates (larger polynomials),
      #   but may be buggy and not trigger mismatches properly.
      expandedEIs = []
      for ei in regex.evilInputs:
        expandedEIs += ei.expand()
      libLF.log("Considering {} EvilInput's, expanded from {}".format(len(expandedEIs), len(regex.evilInputs)))
      evilInputs = expandedEIs
    else:
      evilInputs = regex.evilInputs

    eiWithLargestGrowthRate = None
    eiLargestGrowthRate = -1

    for i, ei in enumerate(evilInputs):
      libLF.log("Checking evil input {}/{}:\n  {}".format(i, len(evilInputs), ei.toNDJSON()))
      pump2meas = {}
      for nPumps in MyTask.MOSTEI_PUMPS:
        queryFile = libMemo.ProtoRegexEngine.buildQueryFile(regex.pattern, ei.build(nPumps)[0])
        try:
          pump2meas[nPumps] = libMemo.ProtoRegexEngine.query(
            libMemo.ProtoRegexEngine.SELECTION_SCHEME.SS_None, libMemo.ProtoRegexEngine.ENCODING_SCHEME.ES_None, queryFile,
            timeout=PROTOTYPE_SL_MATCH_TIMEOUT 
          )
        except subprocess.TimeoutExpired:
          # If we timed out, that's about as significant a growth rate as can be expected
          if not SAVE_TMP_FILES:
            os.unlink(queryFile)
          libLF.log("SL regex: /{}/".format(regex.pattern))
          return ei, GROWTH_RATE_INF

        # libLF.log("{} pumps, {} visits".format(nPumps, pump2meas[nPumps].si_nTotalVisits))
        if not SAVE_TMP_FILES:
          os.unlink(queryFile)
    
      # Compute growth rates -- first derivative
      growthRates = []
      for i_pump, j_pump in zip(MyTask.MOSTEI_PUMPS[1:], MyTask.MOSTEI_PUMPS[2:]):
        growthRates.append( pump2meas[j_pump].si_nTotalVisits - pump2meas[i_pump].si_nTotalVisits )
      
      # If the growth rates are strictly monotonically increasing, then we have super-linear growth
      # (i.e. we're looking for a positive second derivative -- acceleration!)
      growthIsSuperLinear = True
      for g1, g2 in zip(growthRates, growthRates[1:]):
        if g1 >= g2:
          libLF.log('Not super-linear growth. Successive rates were {}, {}'.format(g1, g2))
          growthIsSuperLinear = False
          break
      
      # Is this the largest of the observed growth rates?
      if growthIsSuperLinear:
        largestGrowth = growthRates[-1] - growthRates[-2]
        if largestGrowth > eiLargestGrowthRate:
          eiWithLargestGrowthRate = ei
          eiLargestGrowthRate = largestGrowth

    if eiWithLargestGrowthRate:
      libLF.log("SL regex: /{}/".format(regex.pattern))
    else:
      libLF.log("False SL regex: /{}/".format(regex.pattern))
      with open('nonSL.txt', 'a') as f:
            f.write(regex.pattern + '\n')

    return eiWithLargestGrowthRate, eiLargestGrowthRate

################

def regIsSupportedByPrototype(regex):
  try:
    queryFile = libMemo.ProtoRegexEngine.buildQueryFile(regex.pattern, "a")
    libMemo.ProtoRegexEngine.query(libMemo.ProtoRegexEngine.SELECTION_SCHEME.SS_None, libMemo.ProtoRegexEngine.ENCODING_SCHEME.ES_None, queryFile)
    return True
  except BaseException as err:
    print(err)
    return False

def getTasks(regexFile, perfPumps, maxAttackStringLen, nTrialsPerCondition, taskConfig):
  regexes = loadRegexFile(regexFile)

  # Filter for prototype support
  regexes = [
    reg
    for reg in regexes
    if regIsSupportedByPrototype(reg)
  ]
  libLF.log("{} regexes were supported by prototype".format(len(regexes)))

  tasks = [MyTask(regex, perfPumps, maxAttackStringLen, nTrialsPerCondition, taskConfig) for regex in regexes]
  libLF.log('Prepared {} tasks'.format(len(tasks)))
  return tasks

def loadRegexFile(regexFile):
  """Return a list of libMemo.SimpleRegex's"""
  regexes = []
  libLF.log('Loading regexes from {}'.format(regexFile))
  with open(regexFile, 'r') as inStream:
    for line in inStream:
      line = line.strip()
      if len(line) == 0:
        continue
      
      try:
        # Build a Regex
        regex = libMemo.SimpleRegex()
        regex.initFromNDJSON(line)
        regexes.append(regex)
      except KeyboardInterrupt:
        raise
      except BaseException as err:
        libLF.log('Exception parsing line:\n  {}\n  {}'.format(line, err))
        traceback.print_exc()

    libLF.log('Loaded {} regexes from {}'.format(len(regexes), regexFile))
    return regexes

################

def main(regexFile, useCSharpToFindMostEI, perfPumps, maxAttackStringLen, queryPrototype, runSecurityAnalysis, nTrialsPerCondition, queryProductionEngines, timeSensitive, parallelism, outFile):
  libLF.log('regexFile {} useCSharpToFindMostEI {} perfPumps {} maxAttackStringLen {} queryPrototype {} runSecurityAnalysis {} nTrialsPerCondition {} queryProductionEngines {} timeSensitive {} parallelism {} outFile {}' \
    .format(regexFile, useCSharpToFindMostEI, perfPumps, queryPrototype, maxAttackStringLen, runSecurityAnalysis, nTrialsPerCondition, queryProductionEngines, timeSensitive, parallelism, outFile))

  #### Check dependencies
  libLF.checkShellDependencies(shellDeps)

  #### Load data
  taskConfig = TaskConfig(useCSharpToFindMostEI, queryPrototype, runSecurityAnalysis, queryProductionEngines) 
  tasks = getTasks(regexFile, perfPumps, maxAttackStringLen, nTrialsPerCondition, taskConfig)
  nRegexes = len(tasks)

  #### Collect data
  
  df = None
  nSL = 0
  nNonSL = 0
  nExceptions = 0

  nWorkers = 1 if timeSensitive else parallelism
  libLF.log("timeSensitive {}, so using {} workers".format(timeSensitive, nWorkers))
  results = libLF.parallel.map(tasks, nWorkers,
    libLF.parallel.RateLimitEnums.NO_RATE_LIMIT, libLF.parallel.RateLimitEnums.NO_RATE_LIMIT,
    jitter=False)
  
  if runSecurityAnalysis:
    allSL = [res for res in results if res != MyTask.NOT_SL]
    nSucceeded = len([res for res in results if res])
    nFailed = len([res for res in results if not res])
    libLF.log("{} succeeded in sec'ty analysis, {} failed".format(nSucceeded, nFailed))
    sys.exit(0)
  
  for t, res in zip(tasks, results):
    if type(res) is pd.DataFrame:
      nSL += 1

      if df is None:
        df = res
      else:
        df = df.append(res)
    elif type(res) is type(MyTask.NOT_SL) and res == MyTask.NOT_SL:
      nNonSL += 1
    else:
      libLF.log("Exception on /{}/: {}".format(t.regex.pattern, res))
      nExceptions += 1
  
  libLF.log("{} regexes were SL, {} non-SL, {} exceptions".format(nSL, nNonSL, nExceptions))

  #### Emit results
  libLF.log('Writing results to {}'.format(outFile))
  # df.to_csv(outFile)
  df.to_pickle(outFile)
  libLF.log("Data columns: {}".format(df.columns))

#####################################################

# Parse args
parser = argparse.ArgumentParser(description='Measure the dynamic costs of memoization -- the space and time costs of memoizing this set of regexes, as determined using the prototype engine.')
parser.add_argument('--regex-file', type=str, help='In: NDJSON file of objects containing libMemo.SimpleRegex objects (at least the key "pattern", and "evilInput" if you want an SL-specific analysis)', required=True,
  dest='regexFile')
parser.add_argument('--useCSharpToFindMostEI', help='In: Use CSharp to find the most evil input? Default is to use the prototype engine. This is a way of ensuring that the SL regexes you claim to have protected aren\'t easily eliminated by existing optimizations or aren\'t an artifact of an inefficient prototype', action='store_true', default=False,
  dest='useCSharpToFindMostEI')
parser.add_argument('--perf-pumps', type=int, help='In: Number of pumps to use for performance (Default 20K: Stack Overflow scenario). Try 200K for production engines', required=False, default=20*1000,
  dest='perfPumps')
parser.add_argument('--max-attack-stringLen', type=int, help='In: Max attack string len. Acts as a cap on perf pumps. Is only used in queryPrototype.', required=False, default=-1,
  dest='maxAttackStringLen')
parser.add_argument('--queryPrototype', help='In: Query prototype?', required=False, action='store_true', default=False,
  dest='queryPrototype')
parser.add_argument('--runSecurityAnalysis', action='store_true', help='In: Run the security analysis described in the S&P submission manuscript. Confirm that the total number of simulation position visits increases *linearly* as we increase attack string length. This is true even for finite ambiguity because the factor is a constant function of Q.', required=False, default=False,
  dest='runSecurityAnalysis')
parser.add_argument('--queryProductionEngines', help='In: Test resource cap effectiveness (see Davis dissertation, Chapter 9). Queries other engines (C\#, Perl, PHP) on the SL input to see the effectiveness of resource cap-style defenses', required=False, action='store_true', default=False,
  dest='queryProductionEngines')
parser.add_argument('--trials', type=int, help='In: Number of trials per experimental condition (only for prototype, and affects time costs not complexity)', required=False, default=20,
  dest='nTrialsPerCondition')
parser.add_argument('--time-sensitive', help='In: Is this a time-sensitive analysis? If not, run in parallel', required=False, action='store_true', default=False,
  dest='timeSensitive')
parser.add_argument('--parallelism', type=int, help='Maximum cores to use', required=False, default=libLF.parallel.CPUCount.CPU_BOUND,
  dest='parallelism')
parser.add_argument('--out-file', type=str, help='Out: A pickled dataframe converted from libMemo.MemoizationDynamicAnalysis objects. For best performance, the name should end in .pkl.bz2', required=True,
  dest='outFile')

# Parse args
args = parser.parse_args()

if not args.queryPrototype and not args.runSecurityAnalysis and not args.queryProductionEngines:
  libLF.log("Error, you must request at least one of {--queryPrototype, --runSecurityAnalysis, --queryProductionEngines}")
  sys.exit(1)

# Here we go!
main(args.regexFile, args.useCSharpToFindMostEI, args.perfPumps, args.maxAttackStringLen, args.queryPrototype, args.runSecurityAnalysis, args.nTrialsPerCondition, args.queryProductionEngines, args.timeSensitive, args.parallelism, args.outFile)
