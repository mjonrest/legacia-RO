// Command voice-server relays in-game voice chat for Legacia RO.
//
// The map-server connects to the link port (TCP) and reports which
// characters linked a voice client, with their party, guild, map and position.
// Game clients get a claim code over UDP and send it through their game
// connection, which tells this server which character the endpoint belongs to.
// See doc/voice_chat.md for the full protocol and setup.
package main

import (
	"flag"
	"log"
	"net"
	"os"
	"time"
)

func main() {
	linkAddr := flag.String("link", "127.0.0.1:6950", "TCP address the map-server connects to")
	udpAddr := flag.String("udp", "0.0.0.0:6951", "UDP address game clients send voice to")
	normalRange := flag.Int("range", 14, "cells a player can be heard in Normal chat")
	rate := flag.Float64("rate", 100, "max voice packets per second per player")
	flag.Parse()

	secret := os.Getenv("VOICE_SECRET")
	if len(secret) < 8 || len(secret) > secretLength {
		log.Fatalf("VOICE_SECRET must be set to the same 8-%d character value as voice_secret in map_athena.conf", secretLength)
	}

	hub := NewHub(*normalRange, *rate)

	ln, err := net.Listen("tcp", *linkAddr)
	if err != nil {
		log.Fatal(err)
	}
	ua, err := net.ResolveUDPAddr("udp", *udpAddr)
	if err != nil {
		log.Fatal(err)
	}
	uc, err := net.ListenUDP("udp", ua)
	if err != nil {
		log.Fatal(err)
	}

	udp := NewUDPServer(hub, uc)
	link := NewLinkServer(hub, secret)
	link.OnClaim = udp.SendWelcome
	udp.OnHello = link.AutoQuery
	udp.StatsQuery = link.StatsQuery
	link.OnStats = udp.SendStats
	udp.CellsQuery = link.CellsQuery
	link.OnCells = udp.SendCells
	udp.AurasQuery = link.AurasQuery
	link.OnAuras = udp.SendAuras
	go func() {
		log.Fatal(link.Serve(ln))
	}()
	go func() {
		for range time.Tick(10 * time.Second) {
			hub.Expire(time.Now(), 30*time.Second)
		}
	}()
	go func() {
		for range time.Tick(10 * time.Minute) {
			p, s := hub.Stats()
			log.Printf("%d players registered, %d voice clients connected", p, s)
		}
	}()

	log.Printf("voice-server: map link on %s, voice on udp %s", *linkAddr, *udpAddr)
	log.Fatal(udp.Serve())
}
