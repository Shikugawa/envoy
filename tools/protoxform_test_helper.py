#!/usr/bin/env python3

from run_command import runCommand

import logging
import os
import re
import sys


def PathAndFilename(label):
  """retrieve actual path and filename from bazel label

  Args:
    label: bazel label to specify target proto.

  Returns:
    actual path and filename
  """
  if label.startswith('/'):
    label = label.replace('//', '/', 1)
  elif label.startswith('@'):
    label = re.sub(r'@.*/', '/', label)
  else:
    return label
  label = label.replace(":", "/")
  splitted_label = label.split('/')
  return ['/'.join(splitted_label[:len(splitted_label) - 1]), splitted_label[-1]]


def GoldenProtoFile(path, filename, version):
  """retrieve golden proto file path. In general, those are placed in tools/testdata/protoxform.

  Args:
    path: target proto path
    filename: target proto filename
    version: api version to specify target golden proto filename

  Returns:
    actual golden proto absolute path 
  """
  base = "./"
  base += path + "/" + filename + "." + version + ".gold"
  return os.path.abspath(base)


def ResultProtoFile(path, filename, version):
  """retrieve result proto file path. In general, those are placed in bazel artifacts.

  Args:
    path: target proto path
    filename: target proto filename
    version: api version to specify target result proto filename

  Returns:
    actual result proto absolute path
  """
  base = "./bazel-bin/"
  delimited_path = path.split('/')
  base += os.path.join(*delimited_path, "protos", *delimited_path)
  base += "/{0}.{1}.proto".format(filename, version)
  return os.path.abspath(base)


def Diff(result_file, golden_file):
  """execute diff command with unified form

  Args:
    result_file: result proto file
    golden_file: golden proto file

  Returns:
    output and status code
  """
  command = 'diff -u '
  command += result_file + ' '
  command += golden_file
  status, stdout, stderr = runCommand(command)
  return [status, stdout, stderr]


def Run(path, filename, version):
  """run main execution for protoxform test

  Args:
    path: target proto path
    filename: target proto filename
    version: api version to specify target result proto filename

  Returns:
    result message extracted from diff command
  """
  message = ""
  golden_path = GoldenProtoFile(path, filename, version)
  test_path = ResultProtoFile(path, filename, version)

  status, stdout, stderr = Diff(test_path, golden_path)

  if status != 0:
    message = '\n'.join([str(line) for line in stdout + stderr])

  return message


if __name__ == "__main__":
  messages = ""
  logging.basicConfig(format='%(message)s')
  path, filename = PathAndFilename(sys.argv[1])
  messages += Run(path, filename, 'v2')
  messages += Run(path, filename, 'v3alpha')

  if len(messages) == 0:
    logging.warning("PASS")
  else:
    logging.error("FAILED:\n{}".format(messages))
