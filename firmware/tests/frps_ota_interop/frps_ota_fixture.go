// SPDX-License-Identifier: Apache-2.0
// The server/TLS helpers are the existing exact managed crypto-interop fixture.
package main

import (
 "bufio"
 "context"
 "flag"
 "fmt"
 "net"
 "os"
 "os/exec"
 "strconv"
 "strings"
 "time"
)

func main() {
 peer := flag.String("peer", "", "compiled real C peer")
 image := flag.String("image", "", "complete signed bin")
 sourceImage := flag.String("source-image", "", "distinct source A image")
 scenario := flag.String("scenario", "", "Python through-FRPS scenario")
 target := flag.String("target", "", "exact firmware target")
 mode := flag.String("mode", "success", "success, write_failure, nvs_failure")
 output := flag.String("output", "", "private evidence directory")
 flag.Parse()
 withSessionServer(func(port int, caPath, dir string) {
  ctx, cancel := context.WithTimeout(context.Background(), 180*time.Second)
  defer cancel()
  cmd := exec.CommandContext(ctx, *peer, strconv.Itoa(port), caPath, *image, *sourceImage, *mode, *output)
  stdout, err := cmd.StdoutPipe(); must(err)
  input, err := cmd.StdinPipe(); must(err)
  cmd.Stderr = os.Stderr
  must(cmd.Start())
  reaped := false
  defer func() { if !reaped { _ = cmd.Process.Kill(); _ = cmd.Wait() } }()
  scanner := bufio.NewScanner(stdout)
  if !scanner.Scan() { panic("C peer did not register: " + scanner.Text()) }
  line := scanner.Text()
  if !strings.HasPrefix(line, "READY ") { panic("unexpected peer line: " + line) }
  _, remotePort, err := net.SplitHostPort(strings.TrimPrefix(line, "READY ")); must(err)
  endpoint := "http://127.0.0.1:" + remotePort
  test := exec.CommandContext(ctx, "python3", *scenario, endpoint, *image, *target, *mode, *output)
  test.Stdout, test.Stderr = os.Stdout, os.Stderr
  must(test.Run())
  _, err = input.Write([]byte("STOP\n")); must(err); must(input.Close())
  for scanner.Scan() { fmt.Println(scanner.Text()) }
  must(scanner.Err()); err = cmd.Wait(); reaped = true; must(err)
  // Every reachable consumer connection was made to this official FRPS proxy.
  deadline := time.Now().Add(5*time.Second)
  for {
   connection, err := net.DialTimeout("tcp4", "127.0.0.1:"+remotePort, 100*time.Millisecond)
   if err != nil { break }; _ = connection.Close()
   if time.Now().After(deadline) { panic("official proxy remained after peer stop") }
   time.Sleep(10*time.Millisecond)
  }
  fmt.Println("FRPS_OTA_INTEROP PASS official_frps=0.71.0 loopback_only=true mode="+*mode)
 })
}
